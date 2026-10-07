#pragma once
#include "Channel.hpp"
#include "../base/Logging.hpp"
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <sys/timerfd.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>

using TaskFunc = std::function<void()>;
using ReleaseFunc = std::function<void()>;
class TimerTask{
    private:
        uint64_t _id;       // 定时器任务对象ID
        uint32_t _timeout;  //定时任务的超时时间
        bool _canceled;     // false-表示没有被取消， true-表示被取消
        TaskFunc _task_cb;  //定时器对象要执行的定时任务
        ReleaseFunc _release; //用于删除TimerWheel中保存的定时器对象信息
    public:
        TimerTask(uint64_t id, uint32_t delay, const TaskFunc &cb);
        ~TimerTask();
        void Cancel();
        void SetRelease(const ReleaseFunc &cb);
        uint32_t DelayTime();
};

class TimerWheel {
    private:
        using WeakTask = std::weak_ptr<TimerTask>;
        using PtrTask = std::shared_ptr<TimerTask>;
        int _tick;      //当前的秒针，走到哪里释放哪里，释放哪里，就相当于执行哪里的任务
        int _capacity;  //表盘最大数量---其实就是最大延迟时间
        std::vector<std::vector<PtrTask>> _wheel;
        std::unordered_map<uint64_t, WeakTask> _timers;

        EventLoop *_loop;
        int _timerfd;//定时器描述符--可读事件回调就是读取计数器，执行定时任务
        std::unique_ptr<Channel> _timer_channel;
    private:
        void RemoveTimer(uint64_t id);
        static int CreateTimerfd();
        int ReadTimefd();
        //这个函数应该每秒钟被执行一次，相当于秒针向后走了一步
        void RunTimerTask();
        void OnTime();
        void TimerAddInLoop(uint64_t id, uint32_t delay, const TaskFunc &cb);
        void TimerRefreshInLoop(uint64_t id);
        void TimerCancelInLoop(uint64_t id);
    public:
        TimerWheel(EventLoop *loop);
        ~TimerWheel();
        /*定时器中有个_timers成员，定时器信息的操作有可能在多线程中进行，因此需要考虑线程安全问题*/
        /*如果不想加锁，那就把对定期的所有操作，都放到一个线程中进行*/
        void TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb);
        //刷新/延迟定时任务
        void TimerRefresh(uint64_t id);
        void TimerCancel(uint64_t id);
        /*这个接口存在线程安全问题--这个接口实际上不能被外界使用者调用，只能在模块内，在对应的EventLoop线程内执行*/
        bool HasTimer(uint64_t id);
};
