#include "Channel.hpp"
#include "EventLoop.hpp"

void Channel::Remove() { return _loop->RemoveEvent(this); }
void Channel::Update() { return _loop->UpdateEvent(this); }

Channel::Channel(EventLoop *loop, int fd):_fd(fd), _events(0), _revents(0), _loop(loop) {}

int Channel::Fd() { return _fd; }

uint32_t Channel::Events() { return _events; }

void Channel::SetREvents(uint32_t events) { _revents = events; }

void Channel::SetReadCallback(const EventCallback &cb) { _read_callback = cb; }

void Channel::SetWriteCallback(const EventCallback &cb) { _write_callback = cb; }

void Channel::SetErrorCallback(const EventCallback &cb) { _error_callback = cb; }

void Channel::SetCloseCallback(const EventCallback &cb) { _close_callback = cb; }

void Channel::SetEventCallback(const EventCallback &cb) { _event_callback = cb; }

bool Channel::ReadAble() { return (_events & EPOLLIN); }

bool Channel::WriteAble() { return (_events & EPOLLOUT); }

void Channel::EnableRead() { _events |= EPOLLIN | EPOLLRDHUP; Update(); }

void Channel::EnableWrite() { _events |= EPOLLOUT; Update(); }

void Channel::DisableRead() { _events &= ~(EPOLLIN | EPOLLRDHUP); Update(); }

void Channel::DisableWrite() { _events &= ~EPOLLOUT; Update(); }

void Channel::DisableAll() { _events = 0; Update(); }

void Channel::HandleEvent() {
    if ((_revents & EPOLLIN) || (_revents & EPOLLRDHUP) || (_revents & EPOLLPRI)) {
        /*不管任何事件，都调用的回调函数*/
        if (_read_callback) _read_callback();
    }
    /*有可能会释放连接的操作事件，一次只处理一个*/
    if (_revents & EPOLLOUT) {
        if (_write_callback) _write_callback();
    }else if (_revents & EPOLLERR) {
        if (_error_callback) _error_callback();//一旦出错，就会释放连接，因此要放到前边调用任意回调
    }else if (_revents & EPOLLHUP) {
        if (_close_callback) _close_callback();
    }
    if (_event_callback) _event_callback();
}
