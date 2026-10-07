#include "TimerWheel.hpp"
#include "EventLoop.hpp"
#include <stdexcept>

void TimerWheel::TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb) {
    if (delay == 0 || delay > 60) throw std::invalid_argument("时间轮延迟必须在 1~60 秒内");
    _loop->RunInLoop(std::bind(&TimerWheel::TimerAddInLoop, this, id, delay, cb));
}
//刷新/延迟定时任务
void TimerWheel::TimerRefresh(uint64_t id) {
    _loop->RunInLoop(std::bind(&TimerWheel::TimerRefreshInLoop, this, id));
}
void TimerWheel::TimerCancel(uint64_t id) {
    _loop->RunInLoop(std::bind(&TimerWheel::TimerCancelInLoop, this, id));
}

TimerTask::TimerTask(uint64_t id, uint32_t delay, const TaskFunc &cb): 
    _id(id), _timeout(delay), _task_cb(cb), _canceled(false) {}

TimerTask::~TimerTask() { 
    if (!_canceled && _task_cb) _task_cb();
    if (_release) _release();
}

void TimerTask::Cancel() { _canceled = true; }

void TimerTask::SetRelease(const ReleaseFunc &cb) { _release = cb; }

uint32_t TimerTask::DelayTime() { return _timeout; }

void TimerWheel::RemoveTimer(uint64_t id) {
    auto it = _timers.find(id);
    if (it != _timers.end()) {
        _timers.erase(it);
    }
}

int TimerWheel::CreateTimerfd() {
    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) {
        ERR_LOG("TIMERFD CREATE FAILED!");
        abort();
    }
    //int timerfd_settime(int fd, int flags, struct itimerspec *new, struct itimerspec *old);
    struct itimerspec itime;
    itime.it_value.tv_sec = 1;
    itime.it_value.tv_nsec = 0;//第一次超时时间为1s后
    itime.it_interval.tv_sec = 1; 
    itime.it_interval.tv_nsec = 0; //第一次超时后，每次超时的间隔时
    timerfd_settime(timerfd, 0, &itime, NULL);
    return timerfd;
}

int TimerWheel::ReadTimefd() {
    uint64_t times = 0;
    //有可能因为其他描述符的事件处理花费事件比较长，然后在处理定时器描述符事件的时候，有可能就已经超时了很多次
    //read读取到的数据times就是从上一次read之后超时的次数
    ssize_t ret;
    do { ret = read(_timerfd, &times, sizeof(times)); } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        if (errno == EAGAIN) return 0;
        ERR_LOG("READ TIMEFD FAILED!");
        abort();
    }
    return times;
}

void TimerWheel::RunTimerTask() {
    _tick = (_tick + 1) % _capacity;
    _wheel[_tick].clear();//清空指定位置的数组，就会把数组中保存的所有管理定时器对象的shared_ptr释放掉
}

void TimerWheel::OnTime() {
    //根据实际超时的次数，执行对应的超时任务
    int times = ReadTimefd();
    for (int i = 0; i < times; i++) {
        RunTimerTask();
    }
}

void TimerWheel::TimerAddInLoop(uint64_t id, uint32_t delay, const TaskFunc &cb) {
    if (delay == 0 || delay > 60) throw std::invalid_argument("时间轮延迟必须在 1~60 秒内");
    TimerCancelInLoop(id);
    PtrTask pt(new TimerTask(id, delay, cb));
    pt->SetRelease(std::bind(&TimerWheel::RemoveTimer, this, id));
    int pos = (_tick + delay) % _capacity;
    _wheel[pos].push_back(pt);
    _timers[id] = WeakTask(pt);
}

void TimerWheel::TimerRefreshInLoop(uint64_t id) {
    //通过保存的定时器对象的weak_ptr构造一个shared_ptr出来，添加到轮子中
    auto it = _timers.find(id);
    if (it == _timers.end()) {
        return;//没找着定时任务，没法刷新，没法延迟
    }
    PtrTask pt = it->second.lock();//lock获取weak_ptr管理的对象对应的shared_ptr
    if (!pt) { _timers.erase(it); return; }
    int delay = pt->DelayTime();
    int pos = (_tick + delay) % _capacity;
    _wheel[pos].push_back(pt);
}

void TimerWheel::TimerCancelInLoop(uint64_t id) {
    auto it = _timers.find(id);
    if (it == _timers.end()) {
        return;//没找着定时任务，没法刷新，没法延迟
    }
    PtrTask pt = it->second.lock();
    if (pt) { pt->Cancel(); pt->SetRelease(ReleaseFunc()); }
    _timers.erase(it);
}

TimerWheel::TimerWheel(EventLoop *loop):_capacity(60), _tick(0), _wheel(_capacity), _loop(loop), 
    _timerfd(CreateTimerfd()), _timer_channel(new Channel(_loop, _timerfd)) {
    _timer_channel->SetReadCallback(std::bind(&TimerWheel::OnTime, this));
    _timer_channel->EnableRead();//启动读事件监控
}

bool TimerWheel::HasTimer(uint64_t id) {
    auto it = _timers.find(id);
    if (it == _timers.end()) {
        return false;
    }
    return true;
}

TimerWheel::~TimerWheel() {
    _timer_channel->Remove();
    for (auto &slot : _wheel) {
        for (auto &task : slot) { task->Cancel(); task->SetRelease(ReleaseFunc()); }
    }
    _wheel.clear();
    _timers.clear();
    close(_timerfd);
}
