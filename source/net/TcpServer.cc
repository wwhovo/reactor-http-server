#include "TcpServer.hpp"
#include <sys/signalfd.h>
#include <stdexcept>

void TcpServer::RunAfterInLoop(const Functor &task, int delay) {
    _next_id++;
    _baseloop.TimerAdd(_next_id, delay, task);
}

void TcpServer::NewConnection(int fd) {
    if (_stopping) { close(fd); return; }
    _next_id++;
    PtrConnection conn(new Connection(_pool.NextLoop(), _next_id, fd));
    conn->SetMessageCallback(_message_callback);
    conn->SetClosedCallback(_closed_callback);
    conn->SetConnectedCallback(_connected_callback);
    conn->SetAnyEventCallback(_event_callback);
    conn->SetSrvClosedCallback(std::bind(&TcpServer::RemoveConnection, this, std::placeholders::_1));
    if (_enable_inactive_release) conn->EnableInactiveRelease(_timeout);//启动非活跃超时销毁
    _conns.insert(std::make_pair(_next_id, conn));
    conn->Established();// 先登记，再交给工作线程，避免过早关闭后的遗漏。
}

void TcpServer::RemoveConnectionInLoop(const PtrConnection &conn) {
    uint64_t id = conn->Id();
    auto it = _conns.find(id);
    if (it != _conns.end()) {
        _conns.erase(it);
    }
    if (_stopping && _conns.empty()) _baseloop.Stop();
}

void TcpServer::RemoveConnection(const PtrConnection &conn) {
    _baseloop.RunInLoop(std::bind(&TcpServer::RemoveConnectionInLoop, this, conn));
}

TcpServer::TcpServer(int port, const std::string &ip):
    _next_id(0), _port(port), _timeout(0),
    _enable_inactive_release(false), 
    _acceptor(&_baseloop, port, ip),
    _pool(&_baseloop), _stopping(false), _signal_fd(-1) {
    _acceptor.SetAcceptCallback(std::bind(&TcpServer::NewConnection, this, std::placeholders::_1));
}

TcpServer::~TcpServer() {
    if (_signal_channel) {
        _signal_channel->Remove();
        close(_signal_fd);
        pthread_sigmask(SIG_SETMASK, &_old_signals, NULL);
    }
}

uint16_t TcpServer::Port() { return _acceptor.Port(); }

void TcpServer::EnableSignalStop() {
    _baseloop.AssertInLoop();
    if (_signal_fd >= 0) return;
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, &_old_signals) != 0)
        throw std::runtime_error("阻塞停机信号失败");
    _signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (_signal_fd < 0) {
        pthread_sigmask(SIG_SETMASK, &_old_signals, NULL);
        throw std::runtime_error("创建 signalfd 失败");
    }
    _signal_channel.reset(new Channel(&_baseloop, _signal_fd));
    _signal_channel->SetReadCallback([this]() {
        signalfd_siginfo info;
        while (read(_signal_fd, &info, sizeof(info)) == sizeof(info)) Stop();
    });
    _signal_channel->EnableRead();
}

void TcpServer::Stop(int grace_seconds) {
    if (grace_seconds < 0 || grace_seconds > 60)
        throw std::invalid_argument("停机等待时间必须在 0~60 秒内");
    _baseloop.RunInLoop([this, grace_seconds]() { StopInLoop(grace_seconds); });
}

void TcpServer::StopInLoop(int grace_seconds) {
    if (_stopping) return;
    _stopping = true;
    _acceptor.Stop();
    if (_conns.empty()) { _baseloop.Stop(); return; }
    // 回调可以改变连接表，所以遍历快照。
    std::vector<PtrConnection> conns;
    for (auto &entry : _conns) conns.push_back(entry.second);
    for (auto &conn : conns) {
        if (grace_seconds == 0) conn->Release();
        else conn->Shutdown();
    }
    if (grace_seconds > 0) RunAfterInLoop([this]() {
        for (auto &entry : _conns) entry.second->Release();
    }, grace_seconds);
}

void TcpServer::SetThreadCount(int count) { return _pool.SetThreadCount(count); }

void TcpServer::SetConnectedCallback(const ConnectedCallback&cb) { _connected_callback = cb; }

void TcpServer::SetMessageCallback(const MessageCallback&cb) { _message_callback = cb; }

void TcpServer::SetClosedCallback(const ClosedCallback&cb) { _closed_callback = cb; }

void TcpServer::SetAnyEventCallback(const AnyEventCallback&cb) { _event_callback = cb; }

void TcpServer::EnableInactiveRelease(int timeout) {
    if (timeout < 1 || timeout > 60) throw std::invalid_argument("空闲超时必须在 1~60 秒内");
    _timeout = timeout; _enable_inactive_release = true;
}

void TcpServer::RunAfter(const Functor &task, int delay) {
    if (delay < 1 || delay > 60) throw std::invalid_argument("延迟必须在 1~60 秒内");
    _baseloop.RunInLoop(std::bind(&TcpServer::RunAfterInLoop, this, task, delay));
}

void TcpServer::Start() {
    if (_stopping) return;
    _pool.Create();
    _acceptor.Listen();
    _baseloop.Start();
    _pool.Stop();
}
