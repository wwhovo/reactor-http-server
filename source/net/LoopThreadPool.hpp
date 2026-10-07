#pragma once
#include "EventLoop.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

class LoopThread {
    private:
        /*用于实现_loop获取的同步关系，避免线程创建了，但是_loop还没有实例化之前去获取_loop*/
        std::mutex _mutex;          // 互斥锁
        std::condition_variable _cond;   // 条件变量
        EventLoop *_loop;       // EventLoop指针变量，这个对象需要在线程内实例化
        bool _ready;
        std::thread _thread;    // EventLoop对应的线程
    private:
        /*实例化 EventLoop 对象，唤醒_cond上有可能阻塞的线程，并且开始运行EventLoop模块的功能*/
        void ThreadEntry();
    public:
        /*创建线程，设定线程入口函数*/
        LoopThread();
        ~LoopThread();
        void Stop();
        /*返回当前线程关联的EventLoop对象指针*/
        EventLoop *GetLoop();
};

class LoopThreadPool {
    private:
        int _thread_count;
        int _next_idx;
        EventLoop *_baseloop;
        std::vector<LoopThread*> _threads;
        std::vector<EventLoop *> _loops;
    public:
        LoopThreadPool(EventLoop *baseloop);
        ~LoopThreadPool();
        void SetThreadCount(int count);
        void Create();
        void Stop();
        EventLoop *NextLoop();
};
