#pragma once
#include "Acceptor.hpp"
#include "Connection.hpp"
#include "LoopThreadPool.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <signal.h>

class TcpServer {
    private:
        uint64_t _next_id;      //这是一个自动增长的连接ID，
        int _port;
        int _timeout;           //这是非活跃连接的统计时间---多长时间无通信就是非活跃连接
        bool _enable_inactive_release;//是否启动了非活跃连接超时销毁的判断标志
        EventLoop _baseloop;    //这是主线程的EventLoop对象，负责监听事件的处理
        Acceptor _acceptor;    //这是监听套接字的管理对象
        LoopThreadPool _pool;   //这是从属EventLoop线程池
        std::unordered_map<uint64_t, PtrConnection> _conns;//保存管理所有连接对应的shared_ptr对象

        using ConnectedCallback = std::function<void(const PtrConnection&)>;
        using MessageCallback = std::function<void(const PtrConnection&, Buffer *)>;
        using ClosedCallback = std::function<void(const PtrConnection&)>;
        using AnyEventCallback = std::function<void(const PtrConnection&)>;
        using Functor = std::function<void()>;
        ConnectedCallback _connected_callback;
        MessageCallback _message_callback;
        ClosedCallback _closed_callback;
        AnyEventCallback _event_callback;
        bool _stopping;
        int _signal_fd;
        sigset_t _old_signals;
        std::unique_ptr<Channel> _signal_channel;
    private:
        void RunAfterInLoop(const Functor &task, int delay);
        //为新连接构造一个Connection进行管理
        void NewConnection(int fd);
        void RemoveConnectionInLoop(const PtrConnection &conn);
        //从管理Connection的_conns中移除连接信息
        void RemoveConnection(const PtrConnection &conn);
        void StopInLoop(int grace_seconds);
    public:
        TcpServer(int port, const std::string &ip = "0.0.0.0");
        ~TcpServer();
        uint16_t Port();
        void EnableSignalStop();
        void Stop(int grace_seconds = 3);
        void SetThreadCount(int count);
        void SetConnectedCallback(const ConnectedCallback&cb);
        void SetMessageCallback(const MessageCallback&cb);
        void SetClosedCallback(const ClosedCallback&cb);
        void SetAnyEventCallback(const AnyEventCallback&cb);
        void EnableInactiveRelease(int timeout);
        //用于添加一个定时任务
        void RunAfter(const Functor &task, int delay);
        void Start();
};
