#pragma once
#include "DataManager.hpp" // 包含DataManager和StorageInfo，用于管理文件元数据

#include <cstdint> // 包含标准整数类型
#include <string>
#include <vector>

struct evhttp_request;

extern storage::DataManager* data_; // 外部声明DataManager实例
extern storage::RecycleManager* recycle_data_; // 外部声明RecycleManager实例

namespace storage {
// Service类：实现HTTP服务器的主要逻辑
class Service
{
public:
    // 构造函数：读取服务器配置
    Service();

    // RunModule方法：启动HTTP服务器
    bool RunModule();

private:
    uint16_t server_port_; // 服务器监听端口
    std::string server_ip_; // 服务器IP地址
    std::string download_prefix_; // 下载URL前缀

    // GenHandler：通用的HTTP请求分发器 (静态回调函数)
    static void GenHandler(struct evhttp_request* req, void* arg);

    // Upload：处理文件上传请求
    static void Upload(struct evhttp_request* req, void* arg);

    // TimetoStr：将time_t时间转换为字符串 (此处仅为辅助，实际在ListShow中被generateModernFileList调用)
    static std::string TimetoStr(time_t t);

    // generateModernFileList：生成HTML文件列表片段
    static std::string generateModernFileList(const std::vector<StorageInfo>& files);

    // generateModernRecycleList：生成回收站文件列表片段
    static std::string generateModernRecycleList(const std::vector<StorageInfo>& files);

    // 📁 生成主页面内容
    static std::string generateMainPageContent(const std::vector<StorageInfo>& files);

    // formatSize：格式化文件大小为可读单位 (B, KB, MB, GB)
    static std::string formatSize(uint64_t bytes);

    // ListShow：处理文件列表展示请求
    static void ListShow(struct evhttp_request* req, void* arg);

    // GetETag：根据文件信息生成ETag (用于缓存和断点续传)
    static std::string GetETag(const StorageInfo& info);

    // Download：处理文件下载请求
    static void Download(struct evhttp_request* req, void* arg);

    // Delete：处理文件删除请求
    static void Delete(struct evhttp_request* req, void* arg);

    // Restore: 处理文件恢复请求
    static void Restore(struct evhttp_request* req, void* arg);

    // DeleteRecycle: 处理回收站文件删除请求
    static void DeleteRecycle(struct evhttp_request* req, void* arg);

    // RecycleList: 处理回收站文件列表请求
    static void RecycleList(struct evhttp_request* req, void* arg);

    static void RecycleClear(struct evhttp_request* req, void* arg);
};
}