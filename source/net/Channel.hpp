#pragma once
#include <cstdint>
#include <functional>
#include <sys/epoll.h>

class Poller;
class EventLoop;
class Channel {
    private:
        int _fd;
        EventLoop *_loop;
        uint32_t _events;  // 当前需要监控的事件
        uint32_t _revents; // 当前连接触发的事件
        using EventCallback = std::function<void()>;
        EventCallback _read_callback;   //可读事件被触发的回调函数
        EventCallback _write_callback;  //可写事件被触发的回调函数
        EventCallback _error_callback;  //错误事件被触发的回调函数
        EventCallback _close_callback;  //连接断开事件被触发的回调函数
        EventCallback _event_callback;  //任意事件被触发的回调函数
    public:
        Channel(EventLoop *loop, int fd);
        int Fd();
        uint32_t Events();//获取想要监控的事件
        void SetREvents(uint32_t events);//设置实际就绪的事件
        void SetReadCallback(const EventCallback &cb);
        void SetWriteCallback(const EventCallback &cb);
        void SetErrorCallback(const EventCallback &cb);
        void SetCloseCallback(const EventCallback &cb);
        void SetEventCallback(const EventCallback &cb);
        //当前是否监控了可读
        bool ReadAble(); 
        //当前是否监控了可写
        bool WriteAble();
        //启动读事件监控
        void EnableRead();
        //启动写事件监控
        void EnableWrite();
        //关闭读事件监控
        void DisableRead();
        //关闭写事件监控
        void DisableWrite();
        //关闭所有事件监控
        void DisableAll();
        //移除监控
        void Remove();
        void Update();
        //事件处理，一旦连接触发了事件，就调用这个函数，自己触发了什么事件如何处理自己决定
        void HandleEvent();
};
