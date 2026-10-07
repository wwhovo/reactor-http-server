#include "Acceptor.hpp"
#include <stdexcept>

void Acceptor::HandleRead() {
    if (!_listening) return;
    while (true) {
        int newfd = _socket.Accept();
        if (newfd < 0) return;
        if (_accept_callback) _accept_callback(newfd);
        else close(newfd);
    }
}

int Acceptor::CreateServer(int port, const std::string &ip) {
    if (!_socket.CreateServer(port, ip, true)) {
        throw std::runtime_error("监听 socket 创建失败: " + std::string(strerror(errno)));
    }
    return _socket.Fd();
}

Acceptor::Acceptor(EventLoop *loop, int port, const std::string &ip):
    _socket(), _loop(loop), _channel(loop, CreateServer(port, ip)), _listening(false) {
    _channel.SetReadCallback(std::bind(&Acceptor::HandleRead, this));
}

void Acceptor::SetAcceptCallback(const AcceptCallback &cb) { _accept_callback = cb; }

Acceptor::~Acceptor() { Stop(); }

void Acceptor::Listen() { _listening = true; _channel.EnableRead(); }

void Acceptor::Stop() {
    if (_listening) { _channel.Remove(); _listening = false; }
    _socket.Close();
}

uint16_t Acceptor::Port() { return _socket.LocalPort(); }
