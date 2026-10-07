#pragma once
#include "Buffer.hpp"
#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Socket.hpp"
#include "../base/Any.hpp"
#include "../base/Logging.hpp"
#include <cstdint>
#include <functional>
#include <memory>

class Connection;
//DISCONECTED -- 连接关闭状态；   CONNECTING -- 连接建立成功-待处理状态
//CONNECTED -- 连接建立完成，各种设置已完成，可以通信的状态；  DISCONNECTING -- 待关闭状态
typedef enum { DISCONNECTED, CONNECTING, CONNECTED, DISCONNECTING}ConnStatu;
using PtrConnection = std::shared_ptr<Connection>;
class Connection : public std::enable_shared_from_this<Connection> {
    private:
        uint64_t _conn_id;  // 连接的唯一ID，便于连接的管理和查找
        //uint64_t _timer_id;   //定时器ID，必须是唯一的，这块为了简化操作使用conn_id作为定时器ID
        int _sockfd;        // 连接关联的文件描述符
        bool _enable_inactive_release;  // 连接是否启动非活跃销毁的判断标志，默认为false
        EventLoop *_loop;   // 连接所关联的一个EventLoop
        ConnStatu _statu;   // 连接状态
        Socket _socket;     // 套接字操作管理
        Channel _channel;   // 连接的事件管理
        Buffer _in_buffer;  // 输入缓冲区---存放从socket中读取到的数据
        Buffer _out_buffer; // 输出缓冲区---存放要发送给对端的数据
        Any _context;       // 请求的接收处理上下文

        /*这四个回调函数，是让服务器模块来设置的（其实服务器模块的处理回调也是组件使用者设置的）*/
        /*换句话说，这几个回调都是组件使用者使用的*/
        using ConnectedCallback = std::function<void(const PtrConnection&)>;
        using MessageCallback = std::function<void(const PtrConnection&, Buffer *)>;
        using ClosedCallback = std::function<void(const PtrConnection&)>;
        using AnyEventCallback = std::function<void(const PtrConnection&)>;
        ConnectedCallback _connected_callback;
        MessageCallback _message_callback;
        ClosedCallback _closed_callback;
        AnyEventCallback _event_callback;
        /*组件内的连接关闭回调--组件内设置的，因为服务器组件内会把所有的连接管理起来，一旦某个连接要关闭*/
        /*就应该从管理的地方移除掉自己的信息*/
        ClosedCallback _server_closed_callback;
        uint32_t _read_pauses;
        bool _streaming_output;
        bool _above_high;
        size_t _low_water, _high_water, _output_limit, _peak_output;
        AnyEventCallback _low_water_callback;
    private:
        /*五个channel的事件回调函数*/
        //描述符可读事件触发后调用的函数，接收socket数据放到接收缓冲区中，然后调用_message_callback
        void HandleRead();
        //描述符可写事件触发后调用的函数，将发送缓冲区中的数据进行发送
        void HandleWrite();
        //描述符触发挂断事件
        void HandleClose();
        //描述符触发出错事件
        void HandleError();
        //描述符触发任意事件: 1. 刷新连接的活跃度--延迟定时销毁任务；  2. 调用组件使用者的任意事件回调
        void HandleEvent();
        //连接获取之后，所处的状态下要进行各种设置（启动读监控,调用回调函数）
        void EstablishedInLoop();
        //这个接口才是实际的释放接口
        void ReleaseInLoop();
        //这个接口并不是实际的发送接口，而只是把数据放到了发送缓冲区，启动了可写事件监控
        void SendInLoop(Buffer &buf);
        //这个关闭操作并非实际的连接释放操作，需要判断还有没有数据待处理，待发送
        void ShutdownInLoop();
        //启动非活跃连接超时释放规则
        void EnableInactiveReleaseInLoop(int sec);
        void CancelInactiveReleaseInLoop();
        void UpgradeInLoop(const Any &context, 
                    const ConnectedCallback &conn, 
                    const MessageCallback &msg, 
                    const ClosedCallback &closed, 
                    const AnyEventCallback &event);
    public:
        Connection(EventLoop *loop, uint64_t conn_id, int sockfd);
        ~Connection();
        //获取管理的文件描述符
        int Fd();
        //获取连接ID
        uint64_t Id();
        //是否处于CONNECTED状态
        bool Connected();
        enum ReadPause { Application = 1, OutputPressure = 2, HttpResponsePending = 4 };
        void PauseRead(uint32_t reason = Application);
        void ResumeRead(uint32_t reason = Application);
        bool ReadPaused() const { return _read_pauses != 0; }
        void SetOutputWatermarks(size_t low, size_t high, size_t limit);
        size_t PendingOutput() { _loop->AssertInLoop(); return _out_buffer.ReadAbleSize(); }
        size_t HighWatermark() const { return _high_water; }
        size_t PeakOutput() const { return _peak_output; }
        void SetLowWaterCallback(const AnyEventCallback &callback);
        void BeginStream();
        void EndStream();
        bool StreamWritable() { _loop->AssertInLoop(); return _statu == CONNECTED || (_statu == DISCONNECTING && _streaming_output); }
        EventLoop::Dispatcher Dispatcher() { return _loop->GetDispatcher(); }
        //设置上下文--连接建立完成时进行调用
        void SetContext(const Any &context);
        //获取上下文，返回的是指针
        Any *GetContext();
        void SetConnectedCallback(const ConnectedCallback&cb);
        void SetMessageCallback(const MessageCallback&cb);
        void SetClosedCallback(const ClosedCallback&cb);
        void SetAnyEventCallback(const AnyEventCallback&cb);
        void SetSrvClosedCallback(const ClosedCallback&cb);
        //连接建立就绪后，进行channel回调设置，启动读监控，调用_connected_callback
        void Established();
        //发送数据，将数据放到发送缓冲区，启动写事件监控
        void Send(const char *data, size_t len);
        //提供给组件使用者的关闭接口--并不实际关闭，需要判断有没有数据待处理
        void Shutdown();
        void Release();
        //启动非活跃销毁，并定义多长时间无通信就是非活跃，添加定时任务
        void EnableInactiveRelease(int sec);
        //取消非活跃销毁
        void CancelInactiveRelease();
        //切换协议---重置上下文以及阶段性回调处理函数 -- 而是这个接口必须在EventLoop线程中立即执行
        //防备新的事件触发后，处理的时候，切换任务还没有被执行--会导致数据使用原协议处理了。
        void Upgrade(const Any &context, const ConnectedCallback &conn, const MessageCallback &msg, 
                     const ClosedCallback &closed, const AnyEventCallback &event);
};
