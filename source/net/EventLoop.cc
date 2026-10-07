#include "EventLoop.hpp"

void EventLoop::RunAllTask() {
    std::vector<Functor> functor;
    {
        std::unique_lock<std::mutex> _lock(_mutex);
        _tasks.swap(functor);
    }
    for (auto &f : functor) {
        f();
    }
    return ;
}

int EventLoop::CreateEventFd() {
    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0) {
        ERR_LOG("CREATE EVENTFD FAILED!!");
        abort();//让程序异常退出
    }
    return efd;
}

void EventLoop::ReadEventfd() {
    uint64_t res = 0;
    ssize_t ret;
    do { ret = read(_event_fd, &res, sizeof(res)); } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        //EINTR -- 被信号打断；   EAGAIN -- 表示无数据可读
        if (errno == EINTR || errno == EAGAIN) {
            return;
        }
        ERR_LOG("READ EVENTFD FAILED!");
        abort();
    }
    return ;
}

void EventLoop::WeakUpEventFd() {
    uint64_t val = 1;
    ssize_t ret;
    do { ret = write(_event_fd, &val, sizeof(val)); } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        if (errno == EAGAIN) {
            return;
        }
        ERR_LOG("READ EVENTFD FAILED!");
        abort();
    }
    return ;
}

EventLoop::EventLoop():_thread_id(std::this_thread::get_id()), 
            _event_fd(CreateEventFd()), 
            _event_channel(new Channel(this, _event_fd)),
            _timer_wheel(this), _stopping(false), _dispatch(new DispatchState(this)) {
    //给eventfd添加可读事件回调函数，读取eventfd事件通知次数
    _event_channel->SetReadCallback(std::bind(&EventLoop::ReadEventfd, this));
    //启动eventfd的读事件监控
    _event_channel->EnableRead();
}

EventLoop::~EventLoop() {
    {
        std::lock_guard<std::mutex> lock(_dispatch->mutex);
        _dispatch->loop = NULL;
    }
    _event_channel->Remove();
    close(_event_fd);
}

EventLoop::Dispatcher EventLoop::GetDispatcher() {
    std::weak_ptr<DispatchState> weak = _dispatch;
    return [weak](const std::function<void()> &task) {
        auto state = weak.lock();
        if (!state) return false;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->loop || state->loop->_stopping.load()) return false;
        state->loop->QueueInLoop(task);
        return true;
    };
}

void EventLoop::Stop() {
    std::lock_guard<std::mutex> lock(_dispatch->mutex);
    _stopping.store(true);
    WeakUpEventFd();
}

void EventLoop::Start() {
    AssertInLoop();
    while (!_stopping.load()) {
        //1. 事件监控， 
        std::vector<Channel *> actives;
        _poller.Poll(&actives);
        //2. 事件处理。 
        for (auto &channel : actives) {
            channel->HandleEvent();
        }
        //3. 执行任务
        RunAllTask();
    }
    // Stop 可能早于线程进入 Start；仍须执行已提交的初始化/释放任务。
    while (true) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_tasks.empty()) break;
        }
        RunAllTask();
    }
}

bool EventLoop::IsInLoop() {
    return (_thread_id == std::this_thread::get_id());
}

void EventLoop::AssertInLoop() {
    assert(_thread_id == std::this_thread::get_id());
}

void EventLoop::RunInLoop(const Functor &cb) {
    if (IsInLoop()) {
        return cb();
    }
    return QueueInLoop(cb);
}

void EventLoop::QueueInLoop(const Functor &cb) {
    {
        std::unique_lock<std::mutex> _lock(_mutex);
        _tasks.push_back(cb);
    }
    //唤醒有可能因为没有事件就绪，而导致的epoll阻塞；
    //其实就是给eventfd写入一个数据，eventfd就会触发可读事件
    WeakUpEventFd();
}

void EventLoop::UpdateEvent(Channel *channel) { return _poller.UpdateEvent(channel); }

void EventLoop::RemoveEvent(Channel *channel) { return _poller.RemoveEvent(channel); }

void EventLoop::TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb) { return _timer_wheel.TimerAdd(id, delay, cb); }

void EventLoop::TimerRefresh(uint64_t id) { return _timer_wheel.TimerRefresh(id); }

void EventLoop::TimerCancel(uint64_t id) { return _timer_wheel.TimerCancel(id); }

bool EventLoop::HasTimer(uint64_t id) { return _timer_wheel.HasTimer(id); }
