#include "../source/net/TcpServer.hpp"
#include <atomic>
#include <iostream>

int main() {
    std::atomic<int> closed(0);
    TcpServer server(0, "127.0.0.1");
    server.SetThreadCount(2);
    server.EnableSignalStop();
    server.SetMessageCallback([](const PtrConnection &conn, Buffer *buffer) {
        const std::string message = buffer->ReadAsStringAndPop(buffer->ReadAbleSize());
        if (message == "release-twice") { conn->Release(); conn->Release(); }
        else if (message == "large") {
            const std::string body(8 * 1024 * 1024, 'z');
            conn->Send(body.data(), body.size());
        }
        else conn->Send(message.data(), message.size());
    });
    server.SetClosedCallback([&](const PtrConnection &) { ++closed; });
    std::cout << "LISTENING 127.0.0.1:" << server.Port() << std::endl;
    server.Start();
    std::cout << "STOPPED closed=" << closed.load() << std::endl;
}
