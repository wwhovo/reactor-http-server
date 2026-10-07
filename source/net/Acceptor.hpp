#pragma once
#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Socket.hpp"
#include <cassert>
#include <functional>

class Acceptor {
    private:
        Socket _socket;//用于创建监听套接字
        EventLoop *_loop; //用于对监听套接字进行事件监控
        Channel _channel; //用于对监听套接字进行事件管理

        using AcceptCallback = std::function<void(int)>;
        AcceptCallback _accept_callback;
        bool _listening;
    private:
        /*监听套接字的读事件回调处理函数---获取新连接，调用_accept_callback函数进行新连接处理*/
        void HandleRead();
        int CreateServer(int port, const std::string &ip);
    public:
        /*不能将启动读事件监控，放到构造函数中，必须在设置回调函数后，再去启动*/
        /*否则有可能造成启动监控后，立即有事件，处理的时候，回调函数还没设置：新连接得不到处理，且资源泄漏*/
        Acceptor(EventLoop *loop, int port, const std::string &ip = "0.0.0.0");
        ~Acceptor();
        void SetAcceptCallback(const AcceptCallback &cb);
        void Listen();
        void Stop();
        uint16_t Port();
};
