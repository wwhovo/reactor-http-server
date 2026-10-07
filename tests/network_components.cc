#include "../source/net/Connection.hpp"
#include "../source/net/LoopThreadPool.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <sys/socket.h>
#include <stdexcept>
#include <dirent.h>
#include <signal.h>

static volatile sig_atomic_t interrupted = 0;
static void Interrupt(int) { interrupted = 1; }

static int FdCount() {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) throw std::runtime_error("cannot inspect fd count");
    int count = 0;
    while (readdir(directory)) ++count;
    closedir(directory);
    return count;
}

static void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

static void SocketSemantics() {
    int pair[2];
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    Socket left(pair[0]), right(pair[1]);
    char data[4096] = {};
    Check(left.NonBlockRecv(data, sizeof(data)) == -1 && errno == EAGAIN, "EAGAIN is not EOF");
    Check(right.Send("x", 1) == 1 && left.Recv(data, sizeof(data)) == 1, "read bytes");
    Check(shutdown(right.Fd(), SHUT_WR) == 0 && left.Recv(data, sizeof(data)) == 0, "recv EOF");
    int size = 4096;
    setsockopt(left.Fd(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    ssize_t count;
    do { count = left.NonBlockSend(data, sizeof(data)); } while (count > 0);
    Check(count == -1 && errno == EAGAIN, "nonblocking send EAGAIN");
    right.Close();
    Check(left.NonBlockSend(data, 1) == -1, "closed peer must not raise SIGPIPE");
    left.Close(); left.Close();
    Check(left.Fd() == -1, "idempotent fd close");
    Socket listener, collision;
    Check(listener.CreateServer(0, "127.0.0.1", true), "listen on ephemeral port");
    Check(!collision.CreateServer(listener.LocalPort(), "127.0.0.1", true) && collision.Fd() == -1,
        "bind failure closes fd");
    Socket invalid;
    Check(!invalid.CreateServer(0, "not-an-ip", true) && invalid.Fd() == -1, "invalid bind address");
}

static void HalfCloseAndPartialWrite() {
    int pair[2];
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    int size = 4096;
    setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    EventLoop loop;
    auto conn = std::make_shared<Connection>(&loop, 77, pair[0]);
    const std::string body(1024 * 1024, 'z');
    int closed = 0;
    bool timeout = false;
    std::string received;
    conn->SetConnectedCallback([&](const PtrConnection &c) { c->Send(body.data(), body.size()); });
    conn->SetClosedCallback([&](const PtrConnection &c) {
        ++closed;
        c->Release(); // 再次排队释放也不允许重复回调。
        loop.Stop();
    });
    loop.TimerAdd(99, 3, [&]() { timeout = true; conn->Release(); });
    std::thread reader([&]() {
        Socket peer(pair[1]);
        shutdown(peer.Fd(), SHUT_WR);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        char bytes[8192];
        ssize_t count;
        while ((count = peer.Recv(bytes, sizeof(bytes))) > 0) received.append(bytes, count);
    });
    conn->Established();
    loop.Start();
    reader.join();
    loop.RunAllTask();
    Check(!timeout && received == body && closed == 1, "flush after EOF, partial sends, one close");
}

static void InterruptedRead() {
    struct sigaction action = {}, previous;
    action.sa_handler = Interrupt;
    sigemptyset(&action.sa_mask);
    Check(sigaction(SIGUSR1, &action, &previous) == 0, "install EINTR handler");
    int pair[2];
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    Socket reader(pair[0]), writer(pair[1]);
    std::atomic<bool> ready(false);
    ssize_t result = -1;
    std::thread thread([&]() {
        ready = true;
        char byte;
        result = reader.Recv(&byte, 1);
    });
    while (!ready.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    pthread_kill(thread.native_handle(), SIGUSR1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    writer.Send("x", 1);
    thread.join();
    sigaction(SIGUSR1, &previous, NULL);
    Check(interrupted && result == 1, "recv retries after EINTR");
}

static void DuplicateReleaseAndThreadStop() {
    for (int i = 0; i < 10; ++i) {
        LoopThread thread;
        std::atomic<int> ran(0);
        EventLoop *loop = thread.GetLoop();
        loop->QueueInLoop([&]() { ++ran; });
        // 排在任务之后退出，确保跨线程唤醒与执行。
        loop->QueueInLoop([loop]() { loop->Stop(); });
        thread.Stop();
        Check(ran == 1, "task runs on loop before join");
    }
    int pair[2];
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    Socket peer(pair[1]);
    EventLoop loop;
    int closed = 0;
    auto conn = std::make_shared<Connection>(&loop, 1, pair[0]);
    conn->SetClosedCallback([&](const PtrConnection &) { ++closed; loop.Stop(); });
    conn->Established();
    conn->Release(); conn->Release(); conn->Shutdown();
    loop.Start();
    Check(closed == 1, "duplicate Release/Shutdown");
}

static void SafeDispatchAndWatermarks() {
    EventLoop::Dispatcher dispatch;
    {
        EventLoop loop;
        dispatch = loop.GetDispatcher();
        int ran = 0;
        Check(dispatch([&] { ++ran; }), "live loop accepts dispatch");
        loop.Stop();
        Check(!dispatch([] {}), "stopped loop rejects dispatch");
        loop.Start();
        Check(ran == 1, "accepted dispatch drains on stop");
    }
    Check(!dispatch([] {}), "destroyed loop rejects dispatch");
    int pair[2];
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "watermark socketpair");
    Socket peer(pair[1]);
    EventLoop loop;
    auto conn = std::make_shared<Connection>(&loop, 77, pair[0]);
    conn->Established();
    conn->SetOutputWatermarks(65536, 131072, 262144);
    conn->PauseRead(Connection::Application);
    const std::string chunk(65536, 'x');
    conn->Send(chunk.data(), chunk.size()); conn->Send(chunk.data(), chunk.size());
    Check(conn->ReadPaused() && conn->PeakOutput() == 131072, "high water pauses reader");
    conn->ResumeRead(Connection::Application);
    Check(conn->ReadPaused(), "application resume cannot override output pause");
    bool drained = false;
    conn->SetLowWaterCallback([&](const PtrConnection &c) {
        Check(c->PendingOutput() <= 65536 && !c->ReadPaused(), "low water restores reader");
        drained = true; c->Release();
    });
    conn->SetClosedCallback([&](const PtrConnection &) { loop.Stop(); });
    std::thread reader([&] {
        char bytes[8192];
        while (peer.Recv(bytes, sizeof(bytes)) > 0) {}
    });
    loop.Start(); reader.join();
    Check(drained, "low water callback fired");
}

int main() {
    try {
        const int before = FdCount();
        SocketSemantics(); InterruptedRead(); HalfCloseAndPartialWrite(); DuplicateReleaseAndThreadStop();
        SafeDispatchAndWatermarks();
        Check(FdCount() == before, "epoll/eventfd/timerfd/socket must be closed");
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
