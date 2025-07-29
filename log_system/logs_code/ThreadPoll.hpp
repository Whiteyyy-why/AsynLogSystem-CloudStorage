#pragma once // 防止头文件被多次包含
#include <vector> // 用于存储线程对象
#include <queue> // 用于任务队列
#include <memory> // 用于智能指针
#include <thread> // 用于 std::thread
#include <mutex> // 用于互斥锁
#include <condition_variable> // 用于条件变量
#include <future> // 用于 std::future 和 std::packaged_task
#include <functional> // 用于 std::function
#include <stdexcept> // 用于异常处理

class ThreadPool
{
public:
    // 该函数用于初始化线程池，启动指定数量的线程。
    // 对于每个线程，使用 std::thread 创建一个新线程，并将一个 lambda 函数作为线程的执行体。
    // 在 lambda 函数中，线程会进入一个无限循环，不断尝试从任务队列中获取任务。
    // 使用 std::unique_lock<std::mutex> 加锁，确保线程安全地访问任务队列。
    // 调用 condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); }) 使线程进入等待状态，直到满足以下两个条件之一：线程池停止（stop 为 true）或任务队列不为空。
    // 如果线程池停止且任务队列为空，线程会退出循环。
    // 从任务队列中取出一个任务，并将其移动到局部变量 task 中，然后解锁。
    // 执行取出的任务。
    ThreadPool(size_t threads) // 启动部分线程
        : stop(false) // 初始化停止标志为 false
    {
        for (size_t i = 0; i < threads; ++i) // 循环创建 threads 个线程
        {
            workers.emplace_back( // 向工作线程容器添加新线程
                [this] // 捕获 this 指针，便于访问成员变量
                {
                    for (;;) // 无限循环，线程持续工作
                    {
                        std::function<void()> task; // 定义一个任务对象
                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex); // 加锁，保护任务队列
                            
							this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); }); // 任务队列为空或者线程池停止时，线程进入等待状态（休眠）

                            if (this->stop && this->tasks.empty()) // 如果线程池停止且任务队列为空
                                return; // 退出线程

                            task = std::move(this->tasks.front()); // 取出队首任务
                            this->tasks.pop(); // 移除队首任务
                        }
                        // 执行任务
                        task(); // 调用任务
                    }
                });
        }
    }
    template <class F, class... Args>
    // 该函数用于将一个新任务添加到任务队列中，并返回一个 std::future 对象，用于获取任务的执行结果。
    // 使用 std::packaged_task 将传入的函数 f 和参数 args 打包成一个可调用对象。
    // 通过 task->get_future() 获取一个 std::future 对象，用于异步获取任务的返回值。
    // 使用 std::unique_lock<std::mutex> 加锁，确保线程安全地访问任务队列。
    // 检查线程池是否已经停止，如果停止则抛出异常。
    // 将打包好的任务封装成一个 lambda 函数，并添加到任务队列中。
    // 调用 condition.notify_one() 唤醒一个等待的线程，通知它有新任务可用。
    // 返回 std::future 对象。
    auto enqueue(F&& f, Args &&...args) -> std::future<typename std::result_of<F(Args...)>::type> 
    {
        using return_type = typename std::result_of<F(Args...)>::type; // 定义返回类型

        // 创建一个打包任务
        auto task = std::make_shared<std::packaged_task<return_type()>>( std::bind(std::forward<F>(f), std::forward<Args>(args)...) ); // 绑定参数

        std::future<return_type> res = task->get_future(); // 获取 future
		{ // 创建一个作用域，确保锁在使用后被释放
            std::unique_lock<std::mutex> lock(queue_mutex); // 加锁

            if (stop) // 如果线程池已停止，抛出异常
                throw std::runtime_error("enqueue on stopped ThreadPool");
            // 将任务添加到任务队列
            tasks.emplace([task](){ (*task)(); }); // 将打包任务转为无参 lambda 存入队列
        }
        condition.notify_one(); // 唤醒一个等待线程
        return res; // 返回 future
    }
    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex); // 加锁
            stop = true; // 设置停止标志
        }
        condition.notify_all(); // 唤醒所有线程
        for (std::thread& worker : workers) // 等待所有线程结束
        {
            worker.join(); // 等待线程结束
        }
    }

private:
    std::vector<std::thread> workers;        // 线程们
    std::queue<std::function<void()>> tasks; // 任务队列
    std::mutex queue_mutex;                  // 任务队列的互斥锁
    std::condition_variable condition;       // 条件变量，用于任务队列的同步
    bool stop;
};