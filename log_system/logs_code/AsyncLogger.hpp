#pragma once
#include <atomic> // 用于原子操作，如stop标志
#include <cassert> // 用于断言
#include <cstdarg> // 用于处理可变参数列表，如printf风格的格式化
#include <memory> // 用于智能指针std::shared_ptr
#include <mutex> // 用于互斥锁

#include "Level.hpp" // 日志级别定义
#include "AsyncWorker.hpp" // 异步工作器
#include "Message.hpp" // 日志消息结构
#include "LogFlush.hpp" // 日志刷新器基类及派生类
#include "backlog/CliBackupLog.hpp" // 客户端日志备份逻辑，用于远程备份
#include "ThreadPoll.hpp" // 线程池，用于执行异步备份任务

// 声明外部全局线程池指针
extern ThreadPool* tp;

namespace mylog
{
    // AsyncLogger类是异步日志记录器的核心实现
    class AsyncLogger
    {
    public:
        using ptr = std::shared_ptr<AsyncLogger>; // 定义智能指针类型

        // 构造函数：初始化日志器名称、刷新器列表和异步工作器
        AsyncLogger(const std::string& logger_name, std::vector<LogFlush::ptr>& flushs, AsyncType type)
            : logger_name_(logger_name), // 初始化日志器名称
            flushs_(flushs.begin(), flushs.end()), // 拷贝日志刷新器列表
            // 启动异步工作器，绑定RealFlush方法作为回调，并指定异步类型
            asyncworker(std::make_shared<AsyncWorker>(
                std::bind(&AsyncLogger::RealFlush, this, std::placeholders::_1),
                type)) {}

        virtual ~AsyncLogger() {}; // 虚析构函数，确保派生类资源正确释放

        // 获取日志器名称
        std::string Name() { return logger_name_; }

        // 以下是各种日志级别的记录方法 (Debug, Info, Warn, Error, Fatal)
        // 它们接收文件名、行号和printf风格的格式化字符串及可变参数

        void Debug(const std::string& file, size_t line, const std::string format, ...)
        {
            va_list va; // 可变参数列表
            va_start(va, format); // 初始化可变参数列表
            char* ret;
            // 将格式化字符串和可变参数打印到动态分配的字符串中
            int r = vasprintf(&ret, format.c_str(), va);
            if (r == -1)
                perror("vasprintf failed!!!: ");
            va_end(va); // 结束可变参数列表

            // 将日志信息序列化并推送到异步缓冲区
            serialize(LogLevel::value::DEBUG, file, line, ret);

            free(ret); // 释放vasprintf分配的内存
            ret = nullptr;
        };

        // Info级别日志记录方法 (类似Debug，只是日志级别不同)
        void Info(const std::string& file, size_t line, const std::string format, ...)
        {
            va_list va;
            va_start(va, format);
            char* ret;
            int r = vasprintf(&ret, format.c_str(), va);
            if (r == -1)
                perror("vasprintf failed!!!: ");
            va_end(va);

            serialize(LogLevel::value::INFO, file, line, ret);

            free(ret);
            ret = nullptr;
        };

        // Warn级别日志记录方法
        void Warn(const std::string& file, size_t line, const std::string format, ...)
        {
            va_list va;
            va_start(va, format);
            char* ret;
            int r = vasprintf(&ret, format.c_str(), va);
            if (r == -1)
                perror("vasprintf failed!!!: ");
            va_end(va);

            serialize(LogLevel::value::WARN, file, line, ret);
            free(ret);
            ret = nullptr;
        };

        // Error级别日志记录方法
        void Error(const std::string& file, size_t line, const std::string format, ...)
        {
            va_list va;
            va_start(va, format);
            char* ret;
            int r = vasprintf(&ret, format.c_str(), va);
            if (r == -1)
                perror("vasprintf failed!!!: ");
            va_end(va);

            serialize(LogLevel::value::ERROR, file, line, ret);

            free(ret);
            ret = nullptr;
        };

        // Fatal级别日志记录方法
        void Fatal(const std::string& file, size_t line, const std::string format, ...)
        {
            va_list va;
            va_start(va, format);
            char* ret;
            int r = vasprintf(&ret, format.c_str(), va);
            if (r == -1)
                perror("vasprintf failed!!!: ");
            va_end(va);

            serialize(LogLevel::value::FATAL, file, line, ret);

            free(ret);
            ret = nullptr;
        };

    protected:
        // serialize方法：组织日志消息，并进行必要的备份和推送到缓冲区
        void serialize(LogLevel::value level, const std::string& file, size_t line, char* ret)
        {
            LogMessage msg(level, file, line, logger_name_, ret); // 创建LogMessage对象
            std::string data = msg.format(); // 格式化日志消息为字符串

            // 如果是FATAL或ERROR级别的日志，则将其提交到线程池进行远程备份
            if (level == LogLevel::value::FATAL || level == LogLevel::value::ERROR)
            {
                try
                {
                    // 使用线程池tp异步执行start_backup函数
                    // start_backup是日志备份客户端的入口点
                    auto backup_result_future = tp->enqueue(start_backup, data);
                    backup_result_future.get(); // 阻塞等待备份任务完成（这里可能是为了确保备份的同步性，但异步通常不get()）
                }
                catch (const std::runtime_error& e)
                {
                    // 捕获线程池可能抛出的异常，例如线程池已停止
                    std::cout << __FILE__ << __LINE__ << "thread pool closed" << std::endl;
                }
            }
            // 将格式化后的日志数据推送到异步工作器的缓冲区
            Flush(data.c_str(), data.size());
        }

        // Flush方法：将日志数据推送到异步工作器（线程安全由AsyncWorker内部保证）
        void Flush(const char* data, size_t len)
        {
            asyncworker->Push(data, len); // AsyncWorker的Push函数是线程安全的
        }

        // RealFlush方法：异步工作器回调函数，由异步线程实际执行日志落地
		// 遍历所有注册的日志刷新器，将缓冲区内容刷新到各自的输出目标
        void RealFlush(Buffer& buffer)
        {
            if (flushs_.empty()) // 如果没有注册刷新器，则不做任何操作
                return;
            for (auto& e : flushs_) // 遍历所有注册的刷新器，将缓冲区内容刷新到各自的输出目标
            {
                e->Flush(buffer.Begin(), buffer.ReadableSize());
            }
        }

    protected:
        std::mutex mtx_; // 互斥锁，尽管Push操作由AsyncWorker内部处理线程安全，这里可能用于其他内部状态
        std::string logger_name_; // 日志器名称
        std::vector<LogFlush::ptr> flushs_; // 日志刷新器列表，定义了日志的输出方式
        mylog::AsyncWorker::ptr asyncworker; // 异步工作器实例
    };

    // LoggerBuilder类：用于构建和配置AsyncLogger实例（建造者模式）
    class LoggerBuilder
    {
    public:
        using ptr = std::shared_ptr<LoggerBuilder>; // 定义智能指针类型

        // 设置日志器名称
        void BuildLoggerName(const std::string& name) { logger_name_ = name; }

        // 设置异步模式类型
        void BuildLopperType(AsyncType type) { async_type_ = type; }

        // 添加日志刷新方式（模板方法，支持多种刷新器）
        template <typename FlushType, typename... Args>
        void BuildLoggerFlush(Args &&...args)
        {
            // 使用LogFlushFactory创建指定类型的刷新器并添加到列表中
            flushs_.emplace_back( LogFlushFactory::CreateLog<FlushType>(std::forward<Args>(args)...));
        }

        // 构建并返回一个配置好的AsyncLogger实例
        AsyncLogger::ptr Build()
        {
            assert(logger_name_.empty() == false); // 断言日志器名称不能为空

            // 如果没有指定日志刷新方式，则默认使用标准输出
            if (flushs_.empty())
                flushs_.emplace_back(std::make_shared<StdoutFlush>());

            // 创建并返回AsyncLogger实例
            return std::make_shared<AsyncLogger>( logger_name_, flushs_, async_type_);
        }

    protected:
        std::string logger_name_ = "async_logger"; // 日志器名称，默认值为"async_logger"
        std::vector<mylog::LogFlush::ptr> flushs_; // 存储日志刷新方式
        AsyncType async_type_ = AsyncType::ASYNC_SAFE; // 异步模式类型，默认安全模式
    };
} // namespace mylog

// LoggerBuilder的使用示例
// mylog::LoggerBuilder builder; // 创建LoggerBuilder实例
// builder.BuildLoggerName("my_async_logger"); // 设置日志器名称
// builder.BuildLopperType(mylog::AsyncType::ASYNC_SAFE); // 设置异步类型
// builder.BuildLoggerFlush<mylog::FileFlush>("log.txt"); // 添加文件刷新器
// auto logger = builder.Build(); // 构建AsyncLogger实例
// logger->Info(__FILE__, __LINE__, "This is an info log message with value: %d", 42); // 记录日志