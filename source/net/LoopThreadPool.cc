#include "LoopThreadPool.hpp"
#include <stdexcept>

void LoopThread::ThreadEntry() {
    EventLoop loop;
    {
        std::unique_lock<std::mutex> lock(_mutex);//加锁
        _loop = &loop;
        _ready = true;
        _cond.notify_all();
    }
    loop.Start();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _loop = NULL;
    }
}

LoopThread::LoopThread():_loop(NULL), _ready(false), _thread(&LoopThread::ThreadEntry, this) {}

LoopThread::~LoopThread() { Stop(); }

void LoopThread::Stop() {
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _cond.wait(lock, [this]() { return _ready; });
        if (_loop) _loop->Stop();
    }
    if (_thread.joinable()) _thread.join();
}

EventLoop *LoopThread::GetLoop() {
    EventLoop *loop = NULL;
    {
        std::unique_lock<std::mutex> lock(_mutex);//加锁
        _cond.wait(lock, [&](){ return _ready; });
        loop = _loop;
    }
    return loop;
}

LoopThreadPool::LoopThreadPool(EventLoop *baseloop):_thread_count(0), _next_idx(0), _baseloop(baseloop) {}

LoopThreadPool::~LoopThreadPool() { Stop(); }

void LoopThreadPool::SetThreadCount(int count) {
    if (count < 0 || count > 64) throw std::invalid_argument("线程数必须在 0~64 之间");
    _thread_count = count;
}

void LoopThreadPool::Create() {
    if (!_threads.empty()) return;
    if (_thread_count > 0) {
        _threads.resize(_thread_count);
        _loops.resize(_thread_count);
        try {
            for (int i = 0; i < _thread_count; i++) {
                _threads[i] = new LoopThread();
                _loops[i] = _threads[i]->GetLoop();
            }
        } catch (...) { Stop(); throw; }
    }
    return ;
}

void LoopThreadPool::Stop() {
    for (auto *thread : _threads) delete thread;
    _threads.clear();
    _loops.clear();
}

EventLoop *LoopThreadPool::NextLoop() {
    if (_thread_count == 0) {
        return _baseloop;
    }
    _next_idx = (_next_idx + 1) % _thread_count;
    return _loops[_next_idx];
}
