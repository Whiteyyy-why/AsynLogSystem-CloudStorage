#pragma once
#include "Config.hpp" // 包含Config类，用于获取配置信息，如存储文件路径
#include "Util.hpp"
#include <jsoncpp/json/value.h>
#include <string>
#include <unordered_map> // 包含unordered_map，用于内存中的数据存储
#include <shared_mutex> // 包含shared_mutex，用于读写锁

namespace storage
{
    // 用作初始化存储文件的属性信息
    typedef struct StorageInfo {
        time_t mtime_;
        time_t atime_;
        size_t fsize_;
        std::string storage_path_; // 文件存储路径
        std::string url_;          // 请求URL中的资源路径
        time_t delete_time_ = 0;       // 移至回收站的时间
        std::string origin_type_ = "low";  // 原始存储类型 (如 deep, low)

        bool NewStorageInfo(const std::string& storage_path)
        {
            // 初始化备份文件的信息
            mylog::GetLogger("asynclogger")->Info("NewStorageInfo start");
            FileUtil f(storage_path);
            if (!f.Exists())
            {
                mylog::GetLogger("asynclogger")->Info("file not exists");
                return false;
            }
            mtime_ = f.LastAccessTime();
            atime_ = f.LastModifyTime();
            fsize_ = f.FileSize();
            storage_path_ = storage_path;
            // URL实际就是用户下载文件请求的路径
            // 下载路径前缀+文件名
            storage::Config* config = storage::Config::GetInstance();
            url_ = config->GetDownloadPrefix() + f.FileName();
            mylog::GetLogger("asynclogger")->Info("download_url:%s,mtime_:%s,atime_:%s,fsize_:%d", url_.c_str(), ctime(&mtime_), ctime(&atime_), fsize_);
            mylog::GetLogger("asynclogger")->Info("NewStorageInfo end");
            return true;
        }
    } StorageInfo; // namespace StorageInfo

    // DataManager类：负责管理文件存储信息
    class DataManager
    {
    private:
        std::string storage_file_; // 存储文件信息的文件路径 (如storage.data)
		std::shared_mutex rwlock_; // 读写锁，用于保护table_的并发访问，允许多读单写
        std::unordered_map<std::string, StorageInfo> table_; // 内存中的哈希表，键为URL，值为StorageInfo
        bool need_persist_; // 标志是否需要持久化（在初始化加载期间可能为false）

    public:
        // 构造函数：初始化DataManager，读取配置并加载现有数据
        DataManager()
        {
            mylog::GetLogger("asynclogger")->Info("DataManager construct start"); // 记录日志
            storage_file_ = storage::Config::GetInstance()->GetStorageInfoFile(); // 从Config获取存储文件路径
            need_persist_ = false; // 在加载数据时不进行持久化
            InitLoad(); // 加载现有数据到内存
            need_persist_ = true; // 加载完成后启用持久化
            mylog::GetLogger("asynclogger")->Info("DataManager construct end"); // 记录日志
        }

        // 析构函数：销毁读写锁
        ~DataManager(){}

        // InitLoad方法：程序启动时从文件读取数据到内存
        bool InitLoad()
        {
            mylog::GetLogger("asynclogger")->Info("init datamanager"); // 记录日志
            storage::FileUtil f(storage_file_); // 使用FileUtil操作存储文件
            if (!f.Exists()) { // 如果存储文件不存在，则无需加载
                mylog::GetLogger("asynclogger")->Info("there is no storage file info need to load");
                return true;
            }

            std::string body;
			if (!f.GetContent(&body)) // 读取文件内容 如果f.GetContent失败
                return false;

            // 反序列化文件内容 (JSON格式) 到Json::Value
            Json::Value root;
            storage::JsonUtil::UnSerialize(body, &root);

            // 将反序列化得到的Json::Value中的数据添加到table_中
            for (int i = 0; i < root.size(); i++) // 遍历JSON数组
            {
                StorageInfo info;
                // 从JSON对象中提取各项属性值
                info.fsize_ = root[i]["fsize_"].asInt();
                info.atime_ = root[i]["atime_"].asInt();
                info.mtime_ = root[i]["mtime_"].asInt();
                info.storage_path_ = root[i]["storage_path_"].asString();
                info.url_ = root[i]["url_"].asString();
                Insert(info); // 将加载的StorageInfo插入到内存哈希表（注意，这里的Insert会尝试持久化，但need_persist_此时为false）
            }
            return true;
        }

        // Storage方法：将内存中的数据持久化到文件 (JSON格式)
        bool Storage()
        {
            mylog::GetLogger("asynclogger")->Info("message storage start"); // 记录日志
            std::vector<StorageInfo> arr;
            if (!GetAll(&arr)) // 获取内存中所有StorageInfo
            {
                mylog::GetLogger("asynclogger")->Warn("GetAll fail,can't get StorageInfo");
                return false;
            }

            Json::Value root; // 创建JSON根对象 (Json::Value可以是对象或数组)
            for (auto e : arr) // 遍历所有StorageInfo
            {
                Json::Value item; // 创建JSON子对象
                // 将StorageInfo的属性添加到JSON子对象中
                item["mtime_"] = (Json::Int64)e.mtime_;
                item["atime_"] = (Json::Int64)e.atime_;
                item["fsize_"] = (Json::Int64)e.fsize_;
				item["url_"] = e.url_.c_str();
                item["storage_path_"] = e.storage_path_.c_str();
                root.append(item); // 将子对象添加到根JSON数组中
            }

            // 序列化JSON::Value到字符串
            std::string body;
            mylog::GetLogger("asynclogger")->Info("new message for StorageInfo:%s", body.c_str());
            JsonUtil::Serialize(root, &body);

            // 将序列化后的字符串写入存储文件
            FileUtil f(storage_file_);

            if (f.SetContent(body.c_str(), body.size()) == false) // 使用FileUtil写入文件
                mylog::GetLogger("asynclogger")->Error("SetContent for StorageInfo Error");

            mylog::GetLogger("asynclogger")->Info("message storage end"); // 记录日志
            return true;
        }

        // Insert方法：插入一条新的StorageInfo
        bool Insert(const StorageInfo& info)
        {
            mylog::GetLogger("asynclogger")->Info("data_message Insert start"); // 记录日志
            {
                std::unique_lock<std::shared_mutex> lock(rwlock_); // 使用unique_lock加写锁，确保线程安全
                table_[info.url_] = info; // 插入或更新哈希表项
            }
            // 如果需要持久化，则调用Storage方法写入文件
            if (need_persist_ == true && Storage() == false)
            {
                mylog::GetLogger("asynclogger")->Error("data_message Insert:Storage Error");
                return false;
            }
            mylog::GetLogger("asynclogger")->Info("data_message Insert end"); // 记录日志
            return true;
        }

        // Update方法：更新一条现有的StorageInfo (逻辑与Insert类似，只是语义上是更新)
        bool Update(const StorageInfo& info)
        {
            mylog::GetLogger("asynclogger")->Info("data_message Update start"); // 记录日志
            {
                std::unique_lock<std::shared_mutex> lock(rwlock_); // 加写锁
                table_[info.url_] = info; // 更新哈希表项
            }            
            if (Storage() == false) // 调用Storage方法持久化
            {
                mylog::GetLogger("asynclogger")->Error("data_message Update:Storage Error");
                return false;
            }
            mylog::GetLogger("asynclogger")->Info("data_message Update end"); // 记录日志
            return true;
        }

        // GetOneByURL方法：根据URL（作为键）获取一条StorageInfo
        bool GetOneByURL(const std::string& key, StorageInfo* info)
        {
            std::shared_lock<std::shared_mutex> lock(rwlock_); // 加读锁，允许多个读操作并发
            if (table_.find(key) == table_.end()) // 在哈希表中查找
            {
                return false; // 未找到
            }
            *info = table_[key]; // 找到则拷贝到传入的info指针
            return true;
        }

        // GetOneByStoragePath方法：根据文件实际存储路径获取一条StorageInfo (需要遍历)
        bool GetOneByStoragePath(const std::string& storage_path, StorageInfo* info)
        {
            std::shared_lock<std::shared_mutex> lock(rwlock_); // 加读锁，允许多个读操作并发
            for (auto e : table_) // 遍历哈希表 (比按键查找效率低)
            {
                if (e.second.storage_path_ == storage_path) // 找到匹配的存储路径
                {
                    *info = e.second; // 拷贝信息
                    return true;
                }
            }
            return false; // 未找到
        }

        // GetAll方法：获取内存中所有StorageInfo并添加到vector中
        bool GetAll(std::vector<StorageInfo>* arry)
        {
            std::shared_lock<std::shared_mutex> lock(rwlock_); // 加读锁，允许多个读操作并发
            for (auto e : table_) // 遍历哈希表
                arry->emplace_back(e.second); // 添加到vector中
            return true;
        }

		// Delete方法：删除一条StorageInfo (根据URL)
        bool Delete(const std::string& key)
        {
            mylog::GetLogger("asynclogger")->Info("data_message Delete start"); // 记录日志
            {
                std::unique_lock<std::shared_mutex> lock(rwlock_); // 加写锁，确保线程安全
                if (table_.find(key) == table_.end()) // 如果未找到
                {
                    mylog::GetLogger("asynclogger")->Warn("data_message Delete: key not found");
                    return false; // 返回false表示删除失败
                }
                table_.erase(key); // 删除哈希表项
            }
            if (Storage() == false) // 调用Storage方法持久化
            {
                mylog::GetLogger("asynclogger")->Error("data_message Delete:Storage Error");
                return false;
            }
            mylog::GetLogger("asynclogger")->Info("data_message Delete end"); // 记录日志
            return true; // 返回true表示删除成功
		}
    }; // namespace DataManager

    class RecycleManager
    {
    private:
        std::string recycle_file_; // 回收站文件路径
        std::shared_mutex rwlock_; // 读写锁，保护回收站操作
        std::unordered_map<std::string, StorageInfo> recycle_table_; // 回收站信息表，键为文件URL，值为StorageInfo
        bool need_persist_; // 是否需要持久化回收站数据
    public:
        RecycleManager()
        {
            mylog::GetLogger("asynclogger")->Info("RecycleManager construct start"); // 记录日志
            recycle_file_ = storage::Config::GetInstance()->GetRecycleInfoFile(); // 获取回收站信息文件路径
            need_persist_ = false; // 初始化时不进行持久化
            InitLoad(); // 加载回收站数据
            need_persist_ = true; // 加载完成后启用持久化
            mylog::GetLogger("asynclogger")->Info("RecycleManager construct end"); // 记录日志
        }

        ~RecycleManager(){}

        // InitLoad方法：加载回收站数据
        bool InitLoad()
        {
            mylog::GetLogger("asynclogger")->Info("init recyclemanager"); // 记录日志
            storage::FileUtil f(recycle_file_); // 使用FileUtil操作存储文件
            if (!f.Exists()) { // 如果存储文件不存在，则无需加载
                mylog::GetLogger("asynclogger")->Info("there is no recycle file info need to load");
                return true;
            }

            std::string body;
			if (!f.GetContent(&body)) // 读取文件内容 如果f.GetContent失败
                return false;

            // 反序列化文件内容 (JSON格式) 到Json::Value
            Json::Value root;
            storage::JsonUtil::UnSerialize(body, &root);

            // 将反序列化得到的Json::Value中的数据添加到table_中
            for (int i = 0; i < root.size(); i++) // 遍历JSON数组
            {
                StorageInfo info;
                // 从JSON对象中提取各项属性值
                info.fsize_ = root[i]["fsize_"].asInt();
                info.atime_ = root[i]["atime_"].asInt();
                info.mtime_ = root[i]["mtime_"].asInt();
                info.storage_path_ = root[i]["storage_path_"].asString();
                info.url_ = root[i]["url_"].asString();
                info.delete_time_ = root[i]["delete_time_"].asInt();
                info.origin_type_ = root[i]["origin_type_"].asString();
                Insert(info); // 将加载的StorageInfo插入到内存哈希表（注意，这里的Insert会尝试持久化，但need_persist_此时为false）
            }
            return true;
        }

        // Storage方法：将内存中的数据持久化到文件 (JSON格式)
        bool Storage()
        {
            mylog::GetLogger("asynclogger")->Info("message recycle start"); // 记录日志
            std::vector<StorageInfo> arr;
            if (!GetAll(&arr)) // 获取内存中所有StorageInfo
            {
                mylog::GetLogger("asynclogger")->Warn("GetAll fail,can't get StorageInfo");
                return false;
            }

            Json::Value root; // 创建JSON根对象 (Json::Value可以是对象或数组)
            for (auto e : arr) // 遍历所有StorageInfo
            {
                Json::Value item; // 创建JSON子对象
                // 将StorageInfo的属性添加到JSON子对象中
                item["mtime_"] = (Json::Int64)e.mtime_;
                item["atime_"] = (Json::Int64)e.atime_;
                item["fsize_"] = (Json::Int64)e.fsize_;
				item["url_"] = e.url_.c_str();
                item["storage_path_"] = e.storage_path_.c_str();
                item["delete_time_"] = (Json::Int64)e.delete_time_;
                item["origin_type_"] = e.origin_type_.c_str();
                root.append(item); // 将子对象添加到根JSON数组中
            }

            // 序列化JSON::Value到字符串
            std::string body;
            mylog::GetLogger("asynclogger")->Info("new message for StorageInfo:%s", body.c_str());
            JsonUtil::Serialize(root, &body);

            // 将序列化后的字符串写入存储文件
            FileUtil f(recycle_file_);

            if (f.SetContent(body.c_str(), body.size()) == false) // 使用FileUtil写入文件
                mylog::GetLogger("asynclogger")->Error("SetContent for StorageInfo Error");

            mylog::GetLogger("asynclogger")->Info("message storage end"); // 记录日志
            return true;
        }

        // Insert方法：插入一条新的StorageInfo
        bool Insert(const StorageInfo& info)
        {
            mylog::GetLogger("asynclogger")->Info("data_message Insert start"); // 记录日志
            {
                std::unique_lock<std::shared_mutex> lock(rwlock_); // 使用unique_lock加写锁，确保线程安全
                recycle_table_[info.url_] = info; // 插入或更新哈希表项
            }
            // 如果需要持久化，则调用Storage方法写入文件
            if (need_persist_ == true && Storage() == false)
            {
                mylog::GetLogger("asynclogger")->Error("data_message Insert:Storage Error");
                return false;
            }
            mylog::GetLogger("asynclogger")->Info("data_message Insert end"); // 记录日志
            return true;
        }

        // Update方法：更新一条现有的StorageInfo (逻辑与Insert类似，只是语义上是更新)
        bool Update(const StorageInfo& info)
        {
            mylog::GetLogger("asynclogger")->Info("data_message Update start"); // 记录日志
            {
                std::unique_lock<std::shared_mutex> lock(rwlock_); // 加写锁
                recycle_table_[info.url_] = info; // 更新哈希表项
            }            
            if (Storage() == false) // 调用Storage方法持久化
            {
                mylog::GetLogger("asynclogger")->Error("data_message Update:Storage Error");
                return false;
            }
            mylog::GetLogger("asynclogger")->Info("data_message Update end"); // 记录日志
            return true;
        }

        // GetOneByURL方法：根据URL（作为键）获取一条StorageInfo
        bool GetOneByURL(const std::string& key, StorageInfo* info)
        {
            std::shared_lock<std::shared_mutex> lock(rwlock_); // 加读锁，允许多个读操作并发
            if (recycle_table_.find(key) == recycle_table_.end()) // 在哈希表中查找
            {
                return false; // 未找到
            }
            *info = recycle_table_[key]; // 找到则拷贝到传入的info指针
            return true;
        }

        // GetOneByStoragePath方法：根据文件实际存储路径获取一条StorageInfo (需要遍历)
        bool GetOneByStoragePath(const std::string& storage_path, StorageInfo* info)
        {
            std::shared_lock<std::shared_mutex> lock(rwlock_); // 加读锁，允许多个读操作并发
            for (auto e : recycle_table_) // 遍历哈希表 (比按键查找效率低)
            {
                if (e.second.storage_path_ == storage_path) // 找到匹配的存储路径
                {
                    *info = e.second; // 拷贝信息
                    return true;
                }
            }
            return false; // 未找到
        }

        // GetAll方法：获取内存中所有StorageInfo并添加到vector中
        bool GetAll(std::vector<StorageInfo>* arry)
        {
            std::shared_lock<std::shared_mutex> lock(rwlock_); // 加读锁，允许多个读操作并发
            for (auto e : recycle_table_) // 遍历哈希表
                arry->emplace_back(e.second); // 添加到vector中
            return true;
        }

		// Delete方法：删除一条StorageInfo (根据URL)
        bool Delete(const std::string& key)
        {
            mylog::GetLogger("asynclogger")->Info("data_message Delete start"); // 记录日志
            {
                std::unique_lock<std::shared_mutex> lock(rwlock_); // 加写锁，确保线程安全
                if (recycle_table_.find(key) == recycle_table_.end()) // 如果未找到
                {
                    mylog::GetLogger("asynclogger")->Warn("data_message Delete: key not found");
                    return false; // 返回false表示删除失败
                }
                recycle_table_.erase(key); // 删除哈希表项
            }
            if (Storage() == false) // 调用Storage方法持久化
            {
                mylog::GetLogger("asynclogger")->Error("data_message Delete:Storage Error");
                return false;
            }
            mylog::GetLogger("asynclogger")->Info("data_message Delete end"); // 记录日志
            return true; // 返回true表示删除成功
		}
    };
}   