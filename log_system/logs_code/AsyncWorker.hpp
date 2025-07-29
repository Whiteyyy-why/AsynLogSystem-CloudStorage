#pragma once

#include <atomic> // 用于原子操作，如stop_标志
#include <condition_variable> // 用于线程间的条件等待和通知
#include <functional> // 用于std::function定义回调函数
#include <iostream> // 标准输入输出
#include <mutex> // 用于互斥锁
#include <thread> // 用于创建异步工作线程
#include "AsyncBuffer.hpp" // 包含Buffer类定义

namespace mylog {
    // 定义异步操作类型：安全（阻塞）和不安全（非阻塞/可能丢弃）
    // ASYNC_SAFE：安全模式。写入时如果缓冲区满了，生产者线程会阻塞等待，保证日志不会丢失。
    // ASYNC_UNSAFE：不安全模式。写入时如果缓冲区满了，可能直接丢弃日志，不阻塞生产者线程，追求性能但可能丢日志。
    enum class AsyncType { ASYNC_SAFE, ASYNC_UNSAFE };

    // 定义回调函数类型，接受一个Buffer引用作为参数
    using functor = std::function<void(Buffer&)>;

    // AsyncWorker类，实现日志的异步处理
    class AsyncWorker {
    public:
        using ptr = std::shared_ptr<AsyncWorker>; // 定义智能指针类型

        // 构造函数：初始化异步类型、回调函数，并启动工作线程
        AsyncWorker(const functor& cb, AsyncType async_type = AsyncType::ASYNC_SAFE)
            : async_type_(async_type), // 异步模式
            callback_(cb),       // 日志落地回调函数
            stop_(false),        // 停止标志，初始为false
            // 启动一个新线程，执行ThreadEntry方法作为工作线程
            thread_(std::thread(&AsyncWorker::ThreadEntry, this)) {
        }

        // 析构函数：停止工作线程，确保所有日志被处理
        ~AsyncWorker() { Stop(); }

        // Push方法：生产者线程调用，将日志数据写入缓冲区
        void Push(const char* data, size_t len) {
            std::unique_lock<std::mutex> lock(mtx_); // 加锁保护生产者缓冲区
            // 如果是ASYNC_SAFE模式，且生产者缓冲区空间不足，则等待
            if (AsyncType::ASYNC_SAFE == async_type_) {
                cond_productor_.wait(lock, [&](){ return len <= buffer_productor_.WriteableSize();});
				//std::condition_variable::wait会自动释放锁，等待条件满足后再重新加锁，参数：(锁， 条件)
            }
            // 将数据推入生产者缓冲区
            buffer_productor_.Push(data, len);
            cond_consumer_.notify_one(); // notify_one()：唤醒一个等待的消费者线程，表示有新数据可处理
        }

        // Stop方法：停止工作线程
        void Stop() {
            stop_ = true; // 设置停止标志为true
            cond_consumer_.notify_all(); // 唤醒所有等待的消费者线程，使其检查stop_标志并退出
			thread_.join(); // std::thread::join()会阻塞当前线程，直到工作线程执行完毕
        }

    private:
        // ThreadEntry方法：异步工作线程的入口函数：thread_(std::thread(&AsyncWorker::ThreadEntry, this))
        void ThreadEntry() {
            while (1) {
                { // 缓冲区交换的临界区
                    std::unique_lock<std::mutex> lock(mtx_); // 加锁保护缓冲区交换操作
                    // 如果生产者缓冲区为空且stop_标志为true，等待新数据或停止信号
                    // 否则（有数据或stop_为false），则继续执行
                    if (buffer_productor_.IsEmpty() && stop_) {
                        cond_consumer_.wait(lock, [&]() { return stop_ || !buffer_productor_.IsEmpty();});
                    }  // 有数据则交换，无数据就阻塞
                        
                    // 交换生产者和消费者缓冲区，快速释放锁让生产者继续写入
                    buffer_productor_.Swap(buffer_consumer_);

                    // 如果是ASYNC_SAFE模式，通知生产者线程可以继续写入（缓冲区有空间了）
                    if (async_type_ == AsyncType::ASYNC_SAFE)
                        cond_productor_.notify_one();
                } // 锁在这里被释放

                // 调用回调函数处理消费者缓冲区中的数据（实际的日志落地）
                callback_(buffer_consumer_); 
                buffer_consumer_.Reset(); // 重置消费者缓冲区，准备下一次接收数据

                // 如果停止标志为true且生产者缓冲区也为空，则工作完成，线程退出
                if (stop_ && buffer_productor_.IsEmpty()) return;
            }
        }

    private:
        AsyncType async_type_; // 异步模式类型 (安全/不安全)
        std::atomic<bool> stop_;  // 控制异步工作器是否停止的原子标志 
        // 在软件开发中，原子（atomic） 通常指“原子操作”，即一个操作要么全部完成，要么完全不做，中间不会被其他线程打断。
        // 原子操作是多线程编程中保证数据一致性和线程安全的基础。
        std::mutex mtx_; // 保护共享资源的互斥锁
        mylog::Buffer buffer_productor_; // 生产者缓冲区，用于接收写入的日志
        mylog::Buffer buffer_consumer_; // 消费者缓冲区，用于日志落地
        std::condition_variable cond_productor_; // 生产者条件变量，用于生产者等待缓冲区空间
        std::condition_variable cond_consumer_; // 消费者条件变量，用于消费者等待数据
        std::thread thread_; // 异步工作线程

        functor callback_;  // 回调函数，用于告知工作器如何将日志落地
    };
}  // namespace mylog