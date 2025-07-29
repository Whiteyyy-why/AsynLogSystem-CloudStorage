#pragma once
#include "Util.hpp" // 包含FileUtil和JsonUtil，用于读写配置文件
#include <memory> // 包含智能指针，尽管在这个单例模式中没有直接用于智能指针管理
#include <mutex> // 包含互斥锁，用于单例模式的线程安全初始化

// 该类用于读取配置文件信息
namespace storage
{
    // 定义配置文件的名称
    const char* Config_File = "Storage.conf";

    // Config类以懒汉式单例模式实现，用于管理服务器配置
    class Config
    {
    private:
        // 配置项私有成员变量
        int server_port_;         // 服务器端口
        std::string server_ip;    // 服务器IP
        std::string download_prefix_; // URL路径前缀，用于下载文件
        std::string deep_storage_dir_;    // 深度存储文件的路径
        std::string low_storage_dir_;     // 浅度存储文件的路径
        std::string storage_info_;        // 已存储文件信息的文件路径
        int bundle_format_;               // 深度存储的文件压缩格式

    private:
        // 静态互斥锁，用于保护单例实例的创建
        static std::mutex _mutex;
        // 静态Config实例指针
        static Config* _instance;

        // 私有构造函数，保证只能通过GetInstance()创建实例 (懒汉模式)
        Config()
        {
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Info("Config constructor start"); // 记录开始日志
#endif
            if (ReadConfig() == false) // 尝试读取配置文件
            {
                mylog::GetLogger("asynclogger")->Fatal("ReadConfig failed"); // 读取失败则记录致命错误
                // 这里没有处理错误，而是直接返回，可能导致后续操作使用未初始化的配置
                return;
            }
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Info("ReadConfig complicate"); // 记录完成日志
#endif
        }

    public:
        // 读取配置文件信息的方法
        bool ReadConfig()
        {
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Info("ReadConfig start"); // 记录开始日志
#endif
            storage::FileUtil fu(Config_File); // 使用FileUtil打开配置文件
            std::string content;
            if (!fu.GetContent(&content)) // 读取配置文件内容
            {
                mylog::GetLogger("asynclogger")->Error("Failed to get content from config file: %s", Config_File);
                return false;
            }

            Json::Value root;
            // 反序列化JSON内容到Json::Value对象
            if (!storage::JsonUtil::UnSerialize(content, &root)) { // 使用服务器端的JsonUtil
                mylog::GetLogger("asynclogger")->Error("Failed to deserialize config content.");
                return false;
            }


            // 从JSON根对象中提取各项配置值
            server_port_ = root["server_port"].asInt();
            server_ip = root["server_ip"].asString();
            download_prefix_ = root["download_prefix"].asString();
            storage_info_ = root["storage_info"].asString();
            deep_storage_dir_ = root["deep_storage_dir"].asString();
            low_storage_dir_ = root["low_storage_dir"].asString();
            bundle_format_ = root["bundle_format"].asInt();

            mylog::GetLogger("asynclogger")->Info("ReadConfig finish"); // 记录完成日志
            return true;
        }

        // 以下是各个配置项的public getter方法
        int GetServerPort()
        {
            return server_port_;
        }
        std::string GetServerIp()
        {
            return server_ip;
        }
        std::string GetDownloadPrefix()
        {
            return download_prefix_;
        }
        int GetBundleFormat()
        {
            return bundle_format_;
        }
        std::string GetDeepStorageDir()
        {
            return deep_storage_dir_;
        }
        std::string GetLowStorageDir()
        {
            return low_storage_dir_;
        }
        std::string GetStorageInfoFile()
        {
            return storage_info_;
        }

    public:
        // 获取单例类对象的方法，线程安全
        static Config* GetInstance()
        {
            if (_instance == nullptr) // 双重检查锁定，提高效率
            {
                _mutex.lock(); // 加锁
                if (_instance == nullptr)
                {
                    _instance = new Config(); // 只有在首次访问时创建实例
                }
                _mutex.unlock(); // 解锁
            }
            return _instance;
        }
    };

    // 静态成员初始化
    std::mutex Config::_mutex; // 互斥锁的定义和初始化
    Config* Config::_instance = nullptr; // 单例实例指针的定义和初始化
}