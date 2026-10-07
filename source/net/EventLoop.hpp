#pragma once
#include "Channel.hpp"
#include "Poller.hpp"
#include "TimerWheel.hpp"
#include "../base/Logging.hpp"
#include <cassert>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>

class EventLoop {
    private:
        using Functor = std::function<void()>;
        std::thread::id _thread_id;//线程ID
        int _event_fd;//eventfd唤醒IO事件监控有可能导致的阻塞
        std::unique_ptr<Channel> _event_channel;
        Poller _poller;//进行所有描述符的事件监控
        std::vector<Functor> _tasks;//任务池
        std::mutex _mutex;//实现任务池操作的线程安全
        TimerWheel _timer_wheel;//定时器模块
        std::atomic<bool> _stopping;
        struct DispatchState { std::mutex mutex; EventLoop *loop; explicit DispatchState(EventLoop *p): loop(p) {} };
        std::shared_ptr<DispatchState> _dispatch;
    public:
        //执行任务池中的所有任务
        void RunAllTask();
        static int CreateEventFd();
        void ReadEventfd();
        void WeakUpEventFd();
    public:
        EventLoop();
        ~EventLoop();
        //三步走--事件监控-》就绪事件处理-》执行任务
        void Start();
        void Stop();
        using Dispatcher = std::function<bool(const std::function<void()> &)>;
        // 工作线程使用弱投递句柄，禁止保存 EventLoop 裸指针。
        Dispatcher GetDispatcher();
        //用于判断当前线程是否是EventLoop对应的线程；
        bool IsInLoop();
        void AssertInLoop();
        //判断将要执行的任务是否处于当前线程中，如果是则执行，不是则压入队列。
        void RunInLoop(const Functor &cb);
        //将操作压入任务池
        void QueueInLoop(const Functor &cb);
        //添加/修改描述符的事件监控
        void UpdateEvent(Channel *channel);
        //移除描述符的监控
        void RemoveEvent(Channel *channel);
        void TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb);
        void TimerRefresh(uint64_t id);
        void TimerCancel(uint64_t id);
        bool HasTimer(uint64_t id);
};
