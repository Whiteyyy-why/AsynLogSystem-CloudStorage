#pragma once

#include <memory> // 包含shared_ptr用于智能指针
#include <thread> // 包含thread::get_id用于获取线程ID
#include <sstream> // 包含stringstream用于日志格式化

#include "Level.hpp" // 包含LogLevel定义
#include "Util.hpp"  // 包含Util::Date::Now用于获取时间

namespace mylog
{
    struct LogMessage
    {
        using ptr = std::shared_ptr<LogMessage>;
        LogMessage() = default;
        // 构造函数，初始化日志消息的各个字段
        LogMessage(LogLevel::value level, std::string file, size_t line, std::string name, std::string payload)
            : name_(name),
            file_name_(file),
            payload_(payload),
            level_(level),
            line_(line),
            ctime_(Util::Date::Now()),
            tid_(std::this_thread::get_id()) {}
		std::string format() 
        {   // 格式化日志消息
	        std::stringstream ret; // 创建一个字符串流对象，用于格式化日志消息
            // 获取当前时间
            struct tm t;
            localtime_r(&ctime_, &t); // 将时间戳转换为本地时间
			char buf[128]; // 定义一个字符数组，用于存储格式化后的时间字符串
			strftime(buf, sizeof(buf), "%H:%M:%S", &t); // 将本地时间格式化为字符串，格式为时:分:秒
			std::string tmp1 = '[' + std::string(buf) + "]["; // 构建时间字符串的前半部分
            std::string tmp2 = '[' + std::string(LogLevel::ToString(level_)) + "][" + name_ + "][" + file_name_ + ":" + std::to_string(line_) + "]\t" + payload_ + "\n";
			ret << tmp1 << tid_ << tmp2; // 将时间字符串、线程ID、日志等级、日志器名、文件名、行号和信息体拼接成完整的日志消息
            return ret.str();
            // 日志消息实例：
            // [12:34:56][12345][INFO][MyLogger][main.cpp:42]	日志内容
        }

        size_t line_;           // 行号
        time_t ctime_;          // 时间
        std::string file_name_; // 文件名
        std::string name_;      // 日志器名
        std::string payload_;   // 信息体
        std::thread::id tid_;   // 线程id
        LogLevel::value level_; // 等级
    };
} // namespace mylog