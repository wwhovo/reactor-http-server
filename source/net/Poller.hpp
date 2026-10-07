#pragma once
#include "Channel.hpp"
#include "../base/Logging.hpp"
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/epoll.h>
#include <unordered_map>
#include <vector>

#define MAX_EPOLLEVENTS 1024
class Poller {
    private:
        int _epfd;
        struct epoll_event _evs[MAX_EPOLLEVENTS];
        std::unordered_map<int, Channel *> _channels;
    private:
        //对epoll的直接操作
        void Update(Channel *channel, int op);
        //判断一个Channel是否已经添加了事件监控
        bool HasChannel(Channel *channel);
    public:
        Poller();
        ~Poller();
        //添加或修改监控事件
        void UpdateEvent(Channel *channel);
        //移除监控
        void RemoveEvent(Channel *channel);
        //开始监控，返回活跃连接
        void Poll(std::vector<Channel*> *active);
};
