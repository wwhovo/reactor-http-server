#include "Poller.hpp"
#include <unistd.h>

void Poller::Update(Channel *channel, int op) {
    // int epoll_ctl(int epfd, int op,  int fd,  struct epoll_event *ev);
    int fd = channel->Fd();
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = channel->Events();
    int ret = epoll_ctl(_epfd, op, fd, &ev);
    if (ret < 0) {
        ERR_LOG("EPOLLCTL FAILED!");
    }
    return;
}

bool Poller::HasChannel(Channel *channel) {
    auto it = _channels.find(channel->Fd());
    if (it == _channels.end()) {
        return false;
    }
    return true;
}

Poller::Poller() {
    _epfd = epoll_create1(EPOLL_CLOEXEC);
    if (_epfd < 0) {
        ERR_LOG("EPOLL CREATE FAILED!!");
        abort();//退出程序
    }
}

Poller::~Poller() { close(_epfd); }

void Poller::UpdateEvent(Channel *channel) {
    if (channel->Events() == 0) { RemoveEvent(channel); return; }
    bool ret = HasChannel(channel);
    if (ret == false) {
        //不存在则添加
        _channels.insert(std::make_pair(channel->Fd(), channel));
        return Update(channel, EPOLL_CTL_ADD);
    }
    return Update(channel, EPOLL_CTL_MOD);
}

void Poller::RemoveEvent(Channel *channel) {
    auto it = _channels.find(channel->Fd());
    if (it == _channels.end()) return;
    _channels.erase(it);
    Update(channel, EPOLL_CTL_DEL);
}

void Poller::Poll(std::vector<Channel*> *active) {
    // int epoll_wait(int epfd, struct epoll_event *evs, int maxevents, int timeout)
    int nfds = epoll_wait(_epfd, _evs, MAX_EPOLLEVENTS, -1);
    if (nfds < 0) {
        if (errno == EINTR) {
            return ;
        }
        ERR_LOG("EPOLL WAIT ERROR:%s\n", strerror(errno));
        abort();//退出程序
    }
    for (int i = 0; i < nfds; i++) {
        auto it = _channels.find(_evs[i].data.fd);
        assert(it != _channels.end());
        it->second->SetREvents(_evs[i].events);//设置实际就绪的事件
        active->push_back(it->second);
    }
    return;
}
