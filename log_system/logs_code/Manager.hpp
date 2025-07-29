#include<unordered_map> // 用于存储日志器的哈希表
#include"AsyncLogger.hpp" // 包含AsyncLogger及其Builder
#include <mutex> // 用于互斥锁，保护单例和map访问

namespace mylog {
    // LoggerManager类：通过单例对象对日志器进行管理 (懒汉模式)
    class LoggerManager
    {
    public:
        // 获取LoggerManager的单例实例
        static LoggerManager& GetInstance()
        {
            static LoggerManager eton; // 静态局部变量，线程安全地初始化单例 (C++11保证)
            return eton;
        }

        // 检查指定名称的日志器是否存在
        bool LoggerExist(const std::string& name)
        {
            std::unique_lock<std::mutex> lock(mtx_); // 加锁保护对loggers_的访问
            auto it = loggers_.find(name); // 在哈希表中查找
            if (it == loggers_.end()) // 如果找到，返回true，否则返回false
                return false;
            return true;
        }

        // 添加一个日志器实例
        // 使用右值引用&&接受rvalue，支持移动语义，提高效率
        void AddLogger(const AsyncLogger::ptr&& AsyncLogger)
        {
            if (LoggerExist(AsyncLogger->Name())) // 如果同名日志器已存在，则不添加
                return;
            std::unique_lock<std::mutex> lock(mtx_); // 加锁保护对loggers_的修改
            // 将日志器添加到哈希表中，键为日志器名称，值为日志器智能指针
            loggers_.insert(std::make_pair(AsyncLogger->Name(), AsyncLogger));
        }

        // 根据名称获取日志器实例
        AsyncLogger::ptr GetLogger(const std::string& name)
        {
            std::unique_lock<std::mutex> lock(mtx_); // 加锁保护对loggers_的访问
            auto it = loggers_.find(name);
            if (it == loggers_.end()) // 如果未找到，返回空智能指针
                return AsyncLogger::ptr(); // AsyncLogger::ptr() 等同于 nullptr
            return it->second; // 返回找到的日志器智能指针
        }

        // 获取默认日志器实例
        AsyncLogger::ptr DefaultLogger() { return default_logger_; }

    private:
        // 私有构造函数，保证只能通过GetInstance()创建实例
        LoggerManager()
        {
            // 在构造函数中创建并初始化一个默认日志器
            std::unique_ptr<LoggerBuilder> builder(new LoggerBuilder()); // 使用智能指针管理Builder生命周期
            builder->BuildLoggerName("default"); // 设置默认日志器名称
            default_logger_ = builder->Build(); // 构建默认日志器
            // 将默认日志器添加到管理器的哈希表中
            loggers_.insert(std::make_pair("default", default_logger_));
        }

    private:
        std::mutex mtx_; // 互斥锁，保护loggers_和default_logger_的并发访问
        AsyncLogger::ptr default_logger_; // 存储默认日志器
        // 哈希表，键为日志器名称（std::string），值为日志器智能指针
        std::unordered_map<std::string, AsyncLogger::ptr> loggers_;
    };
}