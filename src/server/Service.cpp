#include "Service.hpp"

#include <event.h>
#include <event2/event.h>
#include <event2/event_struct.h>
#include <evhttp.h> // libevent的HTTP模块
#include <event2/http.h> // libevent的HTTP模块 (新版本路径)

#include <fcntl.h> // 文件控制，如open函数
#include <sys/stat.h> // 文件状态，如open函数
#include <sys/socket.h> // 套接字相关函数
#include <cstring> // 字符串处理
#include <ctime> // 时间处理
#include <fstream> // 文件输入输出
#include <sstream> // 字符串流
#include <regex> // 正则表达式，用于HTML模板替换
#include <iostream> // 输入输出流

#include "Util.hpp"
#include "base64.h" // 来自 cpp-base64 库，用于文件名编码/解码

// 声明外部全局的DataManager指针
extern storage::DataManager* data_;
extern storage::RecycleManager* recycle_data_;

namespace storage {
    
// Service类：实现HTTP服务器的主要逻辑
    // 构造函数：读取服务器配置
Service::Service()
{
#ifdef DEBUG_LOG
    mylog::GetLogger("asynclogger")->Debug("Service start(Construct)"); // 记录调试日志
#endif
    // 从Config单例获取服务器配置
    server_port_ = Config::GetInstance()->GetServerPort();
    server_ip_ = Config::GetInstance()->GetServerIp();
    download_prefix_ = Config::GetInstance()->GetDownloadPrefix();
#ifdef DEBUG_LOG
    mylog::GetLogger("asynclogger")->Debug("Service end(Construct)"); // 记录调试日志
#endif
}

// 自定义 deleter
struct EventBaseDeleter { void operator()(event_base* b) const { if (b) event_base_free(b); } };
struct EvhttpDeleter { void operator()(evhttp* h) const { if (h) evhttp_free(h); } };

// RunModule方法：启动HTTP服务器
bool Service::RunModule() {
    // 初始化libevent事件基础
    std::unique_ptr<event_base, EventBaseDeleter> base(event_base_new());
    if (!base)
    {
        mylog::GetLogger("asynclogger")->Fatal("event_base_new err!"); // 记录致命错误
        return false;
    }

    std::unique_ptr<evhttp, EvhttpDeleter> httpd(evhttp_new(base.get()));
    if (!httpd)
    {
        mylog::GetLogger("asynclogger")->Fatal("evhttp_new err!"); // 记录致命错误
        return false;
    }

    // 绑定HTTP服务器到所有可用IP地址和指定端口
    if (evhttp_bind_socket(httpd.get(), "0.0.0.0", server_port_) != 0)
    {
        mylog::GetLogger("asynclogger")->Fatal("evhttp_bind_socket failed!"); // 记录致命错误
        return false;
    }

    // 设定通用的HTTP请求回调函数
    evhttp_set_gencb(httpd.get(), GenHandler, NULL);

    if(event_base_dispatch(base.get()) == -1) {
        mylog::GetLogger("asynclogger")->Fatal("event_base_dispatch err"); // 记录致命错误
    }

    return true;
}

// GenHandler：通用的HTTP请求分发器 (静态回调函数)
void Service::GenHandler(struct evhttp_request* req, void* arg)
{
    // 获取请求URI路径并进行URL解码
    std::string path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));
    path = UrlDecode(path); // 使用自定义的UrlDecode函数
    mylog::GetLogger("asynclogger")->Info("get req, uri: %s", path.c_str()); // 记录请求URI

    if(evhttp_request_get_command(req) == EVHTTP_REQ_OPTIONS) {
        struct evbuffer* buf = evhttp_request_get_output_buffer(req);
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_OK, "OK", buf);
        return;
    }

    // 根据请求路径判断请求类型并分发
    if (path.find("/download/") != std::string::npos) { // 下载请求
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        Download(req, arg);
    }
    else if (path == "/upload") { // 上传请求
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        Upload(req, arg);
    }
    else if(path == "/delete") { // 删除请求
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        Delete(req, arg);
    }
    else if (path == "/") { // 显示文件列表请求
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        ListShow(req, arg);
    }
    else if(path == "/recycle") { // 显示回收站请求
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        RecycleList(req, arg);
    }
    else if(path == "/recycle/delete") { // 回收站删除请求
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        DeleteRecycle(req, arg);
    }
    else if(path == "/recycle/restore") { // 回收站恢复请求
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        Restore(req, arg);
    }
    else if (path == "/recycle/empty") { // 清空回收站请求
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        RecycleClear(req, arg);
    }
    else { // 未知请求，返回404
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_NOTFOUND, "Not Found", NULL);
    }
}

// Upload：处理文件上传请求
void Service::Upload(struct evhttp_request* req, void* arg) {
    mylog::GetLogger("asynclogger")->Info("Upload start"); // 记录日志

    // 获取请求体缓冲区
    struct evbuffer* buf = evhttp_request_get_input_buffer(req);
    if (buf == nullptr)
    {
        mylog::GetLogger("asynclogger")->Info("evhttp_request_get_input_buffer is empty");
        return;
    }

    size_t len = evbuffer_get_length(buf); // 获取请求体长度
    mylog::GetLogger("asynclogger")->Info("evbuffer_get_length is %u", len);
    if (0 == len) // 如果请求体为空，返回错误
    {
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_BADREQUEST, "file empty", NULL);
        mylog::GetLogger("asynclogger")->Info("request body is empty");
        return;
    }
    std::string content(len, 0); // 创建字符串存储请求体内容
    if (-1 == evbuffer_copyout(buf, (void*)content.c_str(), len)) // 将缓冲区内容复制到字符串
    {
        mylog::GetLogger("asynclogger")->Error("evbuffer_copyout error");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, NULL, NULL); // 服务器内部错误
        return;
    }

    // 获取文件名 (经过Base64编码，在客户端编码)
    std::string filename_encoded = evhttp_find_header(req->input_headers, "FileName");
    // 解码文件名
    std::string filename = base64_decode(filename_encoded);

    // 获取存储类型 (客户端自定义请求头StorageType)
    std::string storage_type = evhttp_find_header(req->input_headers, "StorageType");

    // 组织存储路径
    std::string storage_path_dir;
    if (storage_type == "low") // 普通存储
    {
        storage_path_dir = Config::GetInstance()->GetLowStorageDir();
    }
    else if (storage_type == "deep") // 深度存储
    {
        storage_path_dir = Config::GetInstance()->GetDeepStorageDir();
    }
    else // 非法存储类型
    {
        mylog::GetLogger("asynclogger")->Info("HTTP_BADREQUEST");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_BADREQUEST, "Illegal storage type", NULL);
        return;
    }

    // 如果存储目录不存在，则创建
    FileUtil dirCreate(storage_path_dir);
    dirCreate.CreateDirectory();

    // 完整的最终文件存储路径
    std::string final_storage_path = storage_path_dir + filename;
    std::string base_filename = filename;
    std::string file_extension;
    size_t dot_pos = filename.find_last_of('.');
    if(dot_pos != std::string::npos){
        base_filename = filename.substr(0, dot_pos);
        file_extension = filename.substr(dot_pos);
    }

    int counter = 0;
    do {
        if (counter == 0) {
            final_storage_path = storage_path_dir + filename;
        } else {
            std::stringstream ss;
            ss << storage_path_dir << base_filename;
            if (!file_extension.empty()) {
                ss << "_(" << counter << ")" << file_extension;
            } else {
                ss << "_(" << counter << ")";
            }
            final_storage_path = ss.str();
        }
        
        FileUtil fu_check(final_storage_path);
        if (!fu_check.Exists()) {
            break; // 找到可用的文件名
        }
        
        counter++;
        
        // 防止无限循环（理论上不太可能达到）
        if (counter > 999) {
            // 使用时间戳作为后备方案
            std::stringstream ts_ss;
            ts_ss << storage_path_dir << base_filename << "_" << time(nullptr);
            if (!file_extension.empty()) {
                ts_ss << file_extension;
            }
            final_storage_path = ts_ss.str();
            mylog::GetLogger("asynclogger")->Warn("Used timestamp for unique filename: %s", final_storage_path.c_str());
            break;
        }
    } while (true);
    #ifdef DEBUG_LOG
                mylog::GetLogger("asynclogger")->Debug("storage_path:%s", final_storage_path.c_str());
    #endif

    // 根据存储类型写入文件 (low_storage直接写入，deep_storage压缩后写入)
    FileUtil fu(final_storage_path);
    if (final_storage_path.find("low_storage") != std::string::npos) // 普通存储
    {
        if (fu.SetContent(content.c_str(), len) == false) // 直接写入内容
        {
            mylog::GetLogger("asynclogger")->Error("low_storage fail: HTTP_INTERNAL");
            evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
            evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
            evhttp_send_reply(req, HTTP_INTERNAL, "server error", NULL); // 服务器内部错误
            return;
        }
        else
        {
            mylog::GetLogger("asynclogger")->Info("low_storage success");
        }
    }
    else // 深度存储
    {
        // 压缩内容并写入文件，压缩格式从Config获取
        if (fu.Compress(content, Config::GetInstance()->GetBundleFormat()) == false)
        {
            mylog::GetLogger("asynclogger")->Error("deep_storage fail: HTTP_INTERNAL");
            evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
            evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
            evhttp_send_reply(req, HTTP_INTERNAL, "server error", NULL);
            return;
        }
        else
        {
            mylog::GetLogger("asynclogger")->Info("deep_storage success");
        }
    }

    // 添加存储文件信息到数据管理类
    StorageInfo info;
    info.NewStorageInfo(final_storage_path); // 初始化StorageInfo
    data_->Insert(info); // 向数据管理模块添加信息

    evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
    evhttp_send_reply(req, HTTP_OK, "Success", NULL); // 返回成功响应
    mylog::GetLogger("asynclogger")->Info("upload finish:success");
}

// TimetoStr：将time_t时间转换为字符串 (此处仅为辅助，实际在ListShow中被generateModernFileList调用)
std::string Service::TimetoStr(time_t t) {
    std::string tmp = std::ctime(&t); // 使用ctime将时间戳转为可读字符串
    return tmp;
}

// generateModernFileList：生成HTML文件列表片段
std::string Service::generateModernFileList(const std::vector<StorageInfo>& files) {
    std::stringstream ss;
    ss << "<div class='file-list'><h3>📁 已上传文件 (" << files.size() << " 个)</h3>";

    if (files.empty()) {
        ss << "<div class='empty-state'>"
            << "<div class='icon' style='font-size: 4rem; margin-bottom: 1rem; opacity: 0.5;'>📁</div>"
            << "<h4>还没有上传任何文件</h4>"
            << "<p style='color: #666;'>请选择文件并点击上传按钮</p>"
            << "</div>";
    } else {
        for (const auto& file : files) {
            std::string filename = FileUtil(file.storage_path_).FileName();
            std::string storage_type = "low";
            if (file.storage_path_.find("deep") != std::string::npos) {
                storage_type = "deep";
            }

            ss << "<div class='file-item'>"
                << "<div class='file-info'>"
                << "<span>📄 " << filename << "</span>"
                << "<span class='file-type' style='background: " 
                << (storage_type == "deep" ? "#007bff" : "#28a745") << "; color: white;'>"
                << (storage_type == "deep" ? "深度存储" : "普通存储")
                << "</span>"
                << "<span>" << formatSize(file.fsize_) << "</span>"
                << "<span>" << TimetoStr(file.mtime_) << "</span>"
                << "</div>"
                << "<div class='file-actions' style='display: flex; gap: 0.5rem;'>"
                << "<button onclick=\"window.location='" << file.url_ << "'\" class='btn-primary'>⬇️ 下载</button>"
                << "<button onclick=\"deleteFile('" << file.url_ << "')\" class='btn-warning'>🗑️ 删除</button>"
                << "</div>"
                << "</div>";
        }
    }

    ss << "</div>";
    return ss.str();
}

// generateModernRecycleList：生成回收站文件列表片段
std::string Service::generateModernRecycleList(const std::vector<StorageInfo>& files) {
    std::stringstream ss;

    if (files.empty()) {
        ss << "<div class='empty-state'>"
        << "<div class='icon'>🗑️</div>"
        << "<h4>回收站为空</h4>"
        << "<p>已删除的文件会出现在这里</p>"
        << "<p style='font-size: 0.9rem; color: #999;'>文件删除后会在回收站保留30天</p>"
        << "</div>";
        return ss.str();
    }

    // 统计信息卡片
    size_t total_size = 0;
    for (const auto& file : files) {
        total_size += file.fsize_;
    }

    ss << "<div class='stats-card'>"
    << "<div class='stats-info'>"
    << "📊 统计信息：共 <strong>" << files.size() << "</strong> 个文件，占用 <strong>" << formatSize(total_size) << "</strong>"
    << "</div>"
    << "</div>";

    // 文件列表标题
    ss << "<h3>🗑️ 回收站文件列表</h3>";
    
    // 文件列表
    for (const auto& file : files) {
        std::string filename = FileUtil(file.storage_path_).FileName();
        
        // 移除时间戳前缀
        size_t underscore_pos = filename.find('_');
        if (underscore_pos != std::string::npos) {
            filename = filename.substr(underscore_pos + 1);
        }
        
        // 格式化删除时间
        std::string delete_time_str = "未知时间";
        if (file.delete_time_ > 0) {
            time_t delete_time = file.delete_time_;
            delete_time_str = TimetoStr(delete_time);
            delete_time_str.erase(delete_time_str.find_last_not_of("\n\r") + 1);
        }

        // 计算剩余天数
        int remaining_days = 30 - (time(nullptr) - file.delete_time_) / (24 * 60 * 60);
        if (remaining_days < 0) remaining_days = 0;

        // 存储类型标识
        std::string storage_type = (file.origin_type_ == "low") ? "普通存储" : "深度存储";
        std::string type_color = (file.origin_type_ == "low") ? "#28a745" : "#007bff";

        ss << "<div class='file-item'>"
        << "<div class='file-info'>"
        << "<span>🗑️ " << filename << "</span>"
        << "<span class='file-type' style='background: " << type_color << "; color: white;'>"
        << storage_type << "</span>"
        << "<span>" << formatSize(file.fsize_) << "</span>"
        << "<div class='recycle-meta'>"
        << "<span>🕒 删除于: " << delete_time_str << "</span>"
        << "<span class='" << (remaining_days <= 7 ? "expiry-warning" : "") << "'>⏳ 剩余 " << remaining_days << " 天</span>"
        << "</div>"
        << "</div>"
        << "<div class='file-actions'>"
        << "<button onclick=\"restoreFile('" << file.url_ << "')\" class='btn btn-success'>↩️ 恢复</button>"
        << "<button onclick=\"permanentDelete('" << file.url_ << "')\" class='btn btn-danger'>🗑️ 彻底删除</button>"
        << "</div>"
        << "</div>";
    }

    // 说明卡片
    ss << "<div class='info-card'>"
    << "<h4>📋 回收站说明</h4>"
    << "<ul>"
    << "<li><strong>恢复文件：</strong>将文件恢复到原来的存储位置</li>"
    << "<li><strong>彻底删除：</strong>永久删除文件，无法恢复</li>"
    << "<li><strong>自动清理：</strong>文件在回收站中保留30天后自动清理</li>"
    << "<li><strong>即将过期：</strong>剩余7天及以下的文件会显示红色警告</li>"
    << "</ul>"
    << "</div>";

    return ss.str();
}

// 📁 生成主页面内容
std::string Service::generateMainPageContent(const std::vector<StorageInfo>& files) {
    std::stringstream ss;
    ss << generateModernFileList(files);
    return ss.str();
}

// formatSize：格式化文件大小为可读单位 (B, KB, MB, GB)
std::string Service::formatSize(uint64_t bytes) {
    const char* units[] = { "B", "KB", "MB", "GB" };
    int unit_index = 0;
    double size = bytes;

    while (size >= 1024 && unit_index < 3) // 转换为更大的单位
    {
        size /= 1024;
        unit_index++;
    }

    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << size << " " << units[unit_index]; // 格式化输出
    return ss.str();
}

// ListShow：处理文件列表展示请求
void Service::ListShow(struct evhttp_request* req, void* arg) {
    mylog::GetLogger("asynclogger")->Info("ListShow()"); // 记录日志

    // 1. 获取所有文件存储信息
    std::vector<StorageInfo> arry;
    data_->GetAll(&arry); // 从DataManager获取所有StorageInfo

    // 读取HTML模板文件 (index.html)
    std::ifstream templateFile("index.html");
    std::string templateContent(
        (std::istreambuf_iterator<char>(templateFile)),
        std::istreambuf_iterator<char>()); // 将文件内容读入字符串

    // 替换HTML模板中的占位符
    // 替换文件列表
    templateContent = std::regex_replace(templateContent,
        std::regex("\\{\\{FILE_LIST\\}\\}"), // 查找{{FILE_LIST}}
        generateMainPageContent(arry)); // 替换为生成的文件列表HTML
    // 替换服务器地址
    templateContent = std::regex_replace(templateContent,
        std::regex("\\{\\{BACKEND_URL\\}\\}"), // 查找{{BACKEND_URL}}
        "http://" + storage::Config::GetInstance()->GetServerIp() + ":" + std::to_string(storage::Config::GetInstance()->GetServerPort()));

    // 获取请求的输出缓冲区
    struct evbuffer* buf = evhttp_request_get_output_buffer(req);
    // 将生成的HTML内容添加到输出缓冲区
    evbuffer_add(buf, templateContent.c_str(), templateContent.size());
    evhttp_add_header(req->output_headers, "Content-Type", "text/html;charset=utf-8"); // 设置响应头
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
    evhttp_send_reply(req, HTTP_OK, NULL, NULL); // 发送HTTP OK响应
    mylog::GetLogger("asynclogger")->Info("ListShow() finish"); // 记录日志
}

// GetETag：根据文件信息生成ETag (用于缓存和断点续传)
std::string Service::GetETag(const StorageInfo& info) {
    // 自定义etag格式: filename-fsize-mtime
    FileUtil fu(info.storage_path_);
    std::string etag = fu.FileName();
    etag += "-";
    etag += std::to_string(info.fsize_);
    etag += "-";
    etag += std::to_string(info.mtime_);
    return etag;
}

// Download：处理文件下载请求
void Service::Download(struct evhttp_request* req, void* arg) {
    // 1. 获取请求的资源路径，并获取对应的StorageInfo
    StorageInfo info;
    std::string resource_path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));
    resource_path = UrlDecode(resource_path); // URL解码
    data_->GetOneByURL(resource_path, &info); // 从DataManager获取StorageInfo
    mylog::GetLogger("asynclogger")->Info("request resource_path:%s", resource_path.c_str()); // 记录日志

    std::string download_path = info.storage_path_; // 初始下载路径为文件存储路径
    // 2. 如果是深度存储的文件，则先解压缩到临时目录
    if (info.storage_path_.find(Config::GetInstance()->GetLowStorageDir()) == std::string::npos) // 如果不是low_storage目录
    {
        mylog::GetLogger("asynclogger")->Info("uncompressing:%s", info.storage_path_.c_str()); // 记录日志
        FileUtil fu_compressed(info.storage_path_); // 操作压缩文件
        // 构建解压后的临时文件路径 (在low_storage目录下)
        download_path = Config::GetInstance()->GetLowStorageDir() +
            std::string(download_path.begin() + download_path.find_last_of('/') + 1, download_path.end());
        FileUtil dirCreate(Config::GetInstance()->GetLowStorageDir());
        dirCreate.CreateDirectory(); // 确保low_storage目录存在
        fu_compressed.UnCompress(download_path); // 解压缩文件
    }
    mylog::GetLogger("asynclogger")->Info("request download_path:%s", download_path.c_str()); // 记录日志

    FileUtil fu_download(download_path); // 操作实际下载的文件
    if (fu_download.Exists() == false && info.storage_path_.find("deep_storage") != std::string::npos)
    {
        // 如果是压缩文件，且解压失败导致文件不存在，是服务器错误
        mylog::GetLogger("asynclogger")->Info(": 500 - UnCompress failed");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, NULL, NULL);
    }
    else if (fu_download.Exists() == false && info.storage_path_.find("low_storage") == std::string::npos)
    {
        // 如果是普通文件，且文件不存在，是客户端的请求错误
        mylog::GetLogger("asynclogger")->Info(": 400 - bad request,file not exists");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_BADREQUEST, "file not exists", NULL);
    }

    // 3. 确认文件是否需要断点续传
    bool retrans = false;
    std::string old_etag;
    auto if_range = evhttp_find_header(req->input_headers, "If-Range"); // 获取If-Range头
    if (NULL != if_range)
    {
        old_etag = if_range;
        // 有If-Range字段，并且其值与请求文件的最新ETag一致，则认为是断点续传请求
        if (old_etag == GetETag(info)) // 比较ETag
        {
            retrans = true;
            mylog::GetLogger("asynclogger")->Info("%s need breakpoint continuous transmission", download_path.c_str());
        }
    }

    // 4. 读取文件数据，放入响应体中
    if (fu_download.Exists() == false) // 再次检查文件是否存在 (处理前面判断后的可能性)
    {
        mylog::GetLogger("asynclogger")->Info("%s not exists", download_path.c_str());
        download_path += "not exists"; // 附加信息以便客户端理解
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, 404, download_path.c_str(), NULL); // 返回404
        return;
    }
    evbuffer* outbuf = evhttp_request_get_output_buffer(req); // 获取响应输出缓冲区
    int fd = open(download_path.c_str(), O_RDONLY); // 打开文件以供读取
    if (fd == -1) // 检查文件是否成功打开
    {
        mylog::GetLogger("asynclogger")->Error("open file error: %s -- %s", download_path.c_str(), strerror(errno));
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, strerror(errno), NULL);
        return;
    }
    // 将文件内容添加到输出缓冲区，效率较高 (evbuffer_add_file会直接映射文件)
    if (-1 == evbuffer_add_file(outbuf, fd, 0, fu_download.FileSize()))
    {
        mylog::GetLogger("asynclogger")->Error("evbuffer_add_file: %d -- %s -- %s", fd, download_path.c_str(), strerror(errno));
    }

    // 5. 设置响应头部字段： ETag， Accept-Ranges: bytes
    evhttp_add_header(req->output_headers, "Accept-Ranges", "bytes");
    evhttp_add_header(req->output_headers, "ETag", GetETag(info).c_str());
    evhttp_add_header(req->output_headers, "Content-Type", "application/octet-stream");

    if (retrans == false) // 非断点续传请求
    {
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_OK, "Success", NULL); // 返回200 OK
        mylog::GetLogger("asynclogger")->Info(": HTTP_OK");
    }
    else // 断点续传请求
    {
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, 206, "breakpoint continuous transmission", NULL); // 返回206 Partial Content
        mylog::GetLogger("asynclogger")->Info(": 206");
    }

    // 清理：如果下载路径是临时解压文件，则删除它
    if (download_path != info.storage_path_)
    {
        remove(download_path.c_str()); // 删除文件
    }
}

// Delete：处理文件删除请求
void Service::Delete(struct evhttp_request* req, void* arg) {
    mylog::GetLogger("asynclogger")->Info("Delete start");
    
    std::string url_to_delete;
    // 处理GET请求 - 从URL参数获取
    const char* uri = evhttp_request_get_uri(req);
    mylog::GetLogger("asynclogger")->Info("Delete GET request URI: %s", uri);
    
    // 解析查询参数
    struct evkeyvalq params;
    evhttp_parse_query(uri, &params);
    
    const char* url_param = evhttp_find_header(&params, "url");
    if (url_param) {
        url_to_delete = UrlDecode(url_param); // URL解码
        mylog::GetLogger("asynclogger")->Info("Delete URL from GET params: %s", url_to_delete.c_str());
    }
    evhttp_clear_headers(&params); // 清理参数头

    if (url_to_delete.empty()) {
        mylog::GetLogger("asynclogger")->Error("Delete request missing url parameter");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_BADREQUEST, "Missing url parameter", NULL);
        return;
    }
    
    mylog::GetLogger("asynclogger")->Info("Attempting to delete file with URL: %s", url_to_delete.c_str());
    
    // 从DataManager中获取StorageInfo
    StorageInfo info;
    if (!data_->GetOneByURL(url_to_delete, &info)) {
        mylog::GetLogger("asynclogger")->Error("File not found in DataManager: %s", url_to_delete.c_str());
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_NOTFOUND, "File not found", NULL);
        return;
    }

    std::string recycle_path = Config::GetInstance()->GetRecycleBinDir();
    std::string storage_type = (info.storage_path_.find("low_storage") != std::string::npos) ? "low" : "deep"; // 判断存储类型
    std::string dest_dir = recycle_path + storage_type + "/"; // 回收站目录
    
    FileUtil dirCreate(dest_dir);
    if(!dirCreate.CreateDirectory()){
        mylog::GetLogger("asynclogger")->Error("Failed to create recycle bin directory: %s", dest_dir.c_str());
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, "Failed to create recycle bin directory", NULL);
        return;
    }
    
    std::string filename = FileUtil(info.storage_path_).FileName(); // 获取文件名
    std::string timestamp = std::to_string(time(nullptr)); // 获取当前时间戳
    std::string dest_path = dest_dir + timestamp + "_" + filename; // 设置回收站文件名
    
    // 删除流程，注意安全
    // 回收站文件信息进行insert
    StorageInfo recycle_info = info; // 复制原有信息到回收站信息
    recycle_info.storage_path_ = dest_path; // 设置回收站路径
    recycle_info.delete_time_ = std::stol(timestamp); // 使用与文件名一致的时间戳
    recycle_info.origin_type_ = (storage_type == "low") ? "low" : "deep"; // 设置原始存储类型
    
    if(!recycle_data_->Insert(recycle_info)){
        mylog::GetLogger("asynclogger")->Error("Failed to insert file into recycle bin: %s", url_to_delete.c_str());
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, "Failed to move file to recycle bin", NULL);
        return;
    }

    // 移动物理文件
    if(rename(info.storage_path_.c_str(), dest_path.c_str()) != 0) {
        mylog::GetLogger("asynclogger")->Error("Failed to move file to recycle bin: %s", strerror(errno));
        // 回滚
        recycle_data_->Delete(url_to_delete); // 如果移动失败，删除回收站记录
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, "Failed to move file to recycle bin", NULL);
        return;
    }

    // 删除原来的文件信息
    if(!data_->Delete(url_to_delete)) {
        mylog::GetLogger("asynclogger")->Error("Failed to delete file from DataManager: %s", url_to_delete.c_str());
        // 回滚
        rename(dest_path.c_str(), info.storage_path_.c_str()); // 如果删除失败，恢复文件
        recycle_data_->Delete(url_to_delete); // 删除回收站记录
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, "Failed to delete file from DataManager", NULL);
        return;
    }

    evhttp_add_header(req->output_headers, "Location", "/");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
    evhttp_send_reply(req, 302, "Found", NULL);
    mylog::GetLogger("asynclogger")->Info("File moved to recycle bin, redirecting to main page");
}

// Restore: 处理文件恢复请求
void Service::Restore(struct evhttp_request* req, void* arg) {
    mylog::GetLogger("asynclogger")->Info("Restore start");
    std::string url_to_restore;
    // 处理GET请求 - 从URL参数获取
    const char* uri = evhttp_request_get_uri(req);
    mylog::GetLogger("asynclogger")->Info("Restore GET request URI: %s", uri);
    
    // 解析参数
    struct evkeyvalq params;
    evhttp_parse_query(uri, &params);

    const char* url_param = evhttp_find_header(&params, "url");
    if(url_param) {
        url_to_restore = UrlDecode(url_param);
        mylog::GetLogger("asynclogger")->Info("Restore URL from GET params: %s", url_to_restore.c_str());
    }
    evhttp_clear_headers(&params);

    if(url_to_restore.empty()){
        mylog::GetLogger("asynclogger")->Error("Restore request missing url parameter");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_BADREQUEST, "Missing url parameter", NULL);
        return;
    }

    mylog::GetLogger("asynclogger")->Info("Attempting to restore file with URL: %s", url_to_restore.c_str());

    // 从回收站获取StorageInfo
    StorageInfo info;
    if (!recycle_data_->GetOneByURL(url_to_restore, &info)) {
        mylog::GetLogger("asynclogger")->Error("Failed to get file info from recycle bin: %s", url_to_restore.c_str());
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_NOTFOUND, "File not found in recycle bin", NULL);
        return;
    }
    mylog::GetLogger("asynclogger")->Info("Restoring file: %s", info.storage_path_.c_str());

    // 确定目标存储路径
    std::string storage_type = (info.origin_type_ == "low") ? Config::GetInstance()->GetLowStorageDir() : Config::GetInstance()->GetDeepStorageDir();
    std::string dest_path = storage_type + FileUtil(info.storage_path_).FileName(); // 恢复到原存储目录
    StorageInfo new_info = info; // 创建新的StorageInfo用于恢复
    new_info.storage_path_ = dest_path; // 设置恢复后的存储路径
    new_info.delete_time_ = 0; // 清除删除时间
    new_info.origin_type_ = info.origin_type_; // 恢复原始存储类型

    if(!data_->Insert(new_info)) {
        mylog::GetLogger("asynclogger")->Error("Failed to insert restored file into DataManager: %s", url_to_restore.c_str());
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, "Failed to restore file", NULL);
        return;
    }

    if(rename(info.storage_path_.c_str(), dest_path.c_str()) != 0) {
        mylog::GetLogger("asynclogger")->Error("Failed to restore file: %s", strerror(errno));
        data_->Delete(url_to_restore); // 回滚，删除新插入的记录
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, "Failed to restore file", NULL);
        return;
    }

    if(!recycle_data_->Delete(url_to_restore)) {
        mylog::GetLogger("asynclogger")->Error("Failed to delete file from recycle bin: %s", url_to_restore.c_str());
        rename(dest_path.c_str(), info.storage_path_.c_str()); // 回滚，恢复文件
        data_->Delete(url_to_restore); // 删除新插入的记录
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, "Failed to delete file from recycle bin", NULL);
        return;
    }

    evhttp_add_header(req->output_headers, "Location", "/recycle");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
    evhttp_send_reply(req, 302, "Found", NULL);
    mylog::GetLogger("asynclogger")->Info("File restored, redirecting to recycle page");
}

// DeleteRecycle: 处理回收站文件删除请求
void Service::DeleteRecycle(struct evhttp_request* req, void* arg) {
    mylog::GetLogger("asynclogger")->Info("DeleteRecycle start");
    std::string url_to_delete;
    // 处理GET请求 - 从URL参数获取
    const char* uri = evhttp_request_get_uri(req);
    mylog::GetLogger("asynclogger")->Info("DeleteRecycle GET request URI: %s", uri);
    
    // 解析参数
    struct evkeyvalq params;
    evhttp_parse_query(uri, &params);

    const char* url_param = evhttp_find_header(&params, "url");
    if(url_param) {
        url_to_delete = UrlDecode(url_param);
        mylog::GetLogger("asynclogger")->Info("DeleteRecycle URL from GET params: %s", url_to_delete.c_str());
    }
    evhttp_clear_headers(&params);

    if(url_to_delete.empty()){
        mylog::GetLogger("asynclogger")->Error("DeleteRecycle request missing url parameter");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_BADREQUEST, "Missing url parameter", NULL);
        return;
    }

    mylog::GetLogger("asynclogger")->Info("Attempting to delete file with URL: %s", url_to_delete.c_str());

    // 从回收站获取StorageInfo
    StorageInfo info;
    if (!recycle_data_->GetOneByURL(url_to_delete, &info)) {
        mylog::GetLogger("asynclogger")->Error("Failed to get file info from recycle bin: %s", url_to_delete.c_str());
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_NOTFOUND, "File not found in recycle bin", NULL);
        return;
    }
    mylog::GetLogger("asynclogger")->Info("Delete file: %s", info.storage_path_.c_str());

    // 删除物理文件
    if(remove(info.storage_path_.c_str()) != 0) {
        mylog::GetLogger("asynclogger")->Error("Failed to delete file: %s", strerror(errno));
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, "Failed to delete file", NULL);
        return;
    }

    // 从回收站删除记录
    if(!recycle_data_->Delete(url_to_delete)) {
        mylog::GetLogger("asynclogger")->Error("Failed to delete file from recycle bin: %s", url_to_delete.c_str());
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
        evhttp_send_reply(req, HTTP_INTERNAL, "Failed to delete file from recycle bin", NULL);
        return;
    }

    evhttp_add_header(req->output_headers, "Location", "/recycle");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
    evhttp_send_reply(req, 302, "Found", NULL); // 重定向
    mylog::GetLogger("asynclogger")->Info("File permanently deleted, redirecting to recycle page");
}

// RecycleList: 处理回收站文件列表请求
void Service::RecycleList(struct evhttp_request* req, void* arg) {
    mylog::GetLogger("asynclogger")->Info("RecycleList() - Recycle page"); // 记录日志

    // 1. 获取所有文件存储信息
    std::vector<StorageInfo> recycle_files;
    recycle_data_->GetAll(&recycle_files); // 从DataManager获取所有StorageInfo

    // 读取HTML模板文件 (index.html)
    std::ifstream templateFile("recycle.html");
    std::string templateContent(
        (std::istreambuf_iterator<char>(templateFile)),
        std::istreambuf_iterator<char>()); // 将文件内容读入字符串

    // 替换HTML模板中的占位符
    // 替换文件列表
    templateContent = std::regex_replace(templateContent,
        std::regex("\\{\\{RECYCLE_CONTENT\\}\\}"), // 查找{{RECYCLE_LIST}}
        generateModernRecycleList(recycle_files)); // 替换为生成的文件列表HTML
    // 替换服务器地址
    templateContent = std::regex_replace(templateContent,
        std::regex("\\{\\{BACKEND_URL\\}\\}"), // 查找{{BACKEND_URL}}
        "http://" + storage::Config::GetInstance()->GetServerIp() + ":" + std::to_string(storage::Config::GetInstance()->GetServerPort()));

    // 获取请求的输出缓冲区
    struct evbuffer* buf = evhttp_request_get_output_buffer(req);
    // 将生成的HTML内容添加到输出缓冲区
    evbuffer_add(buf, templateContent.c_str(), templateContent.size());
    evhttp_add_header(req->output_headers, "Content-Type", "text/html;charset=utf-8"); // 设置响应头
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
    evhttp_send_reply(req, HTTP_OK, NULL, NULL); // 发送HTTP OK响应
    mylog::GetLogger("asynclogger")->Info("RecycleList() finish"); // 记录日志
}

void Service::RecycleClear(struct evhttp_request* req, void* arg) {
    mylog::GetLogger("asynclogger")->Info("RecycleClean() - Cleaning up recycle bin");
    
    // 1. 获取所有文件存储信息
    std::vector<StorageInfo> recycle_files;
    recycle_data_->GetAll(&recycle_files); // 从DataManager获取所有StorageInfo

    // 2. 遍历回收站文件，执行清理操作
    for (const auto& file : recycle_files) {
        mylog::GetLogger("asynclogger")->Info("Deleting file from recycle bin: %s", file.storage_path_.c_str());
        if(remove(file.storage_path_.c_str()) != 0) {
            mylog::GetLogger("asynclogger")->Error("Failed to delete file: %s", strerror(errno));
            evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
            evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
            evhttp_send_reply(req, HTTP_INTERNAL, "Failed to delete file", NULL);
            return;
        }

        if(!recycle_data_->Delete(file.url_)) {
            mylog::GetLogger("asynclogger")->Error("Failed to delete file from recycle bin: %s", file.url_.c_str());
            evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
            evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
            evhttp_send_reply(req, HTTP_INTERNAL, "Failed to delete file from recycle bin", NULL);
            return;
        }
    }

    // 3. 清理完成，返回响应
    evhttp_add_header(req->output_headers, "Location", "/recycle");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");
    evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", "content-type,filename,storagetype");
    evhttp_send_reply(req, 302, "Found", NULL); // 重定向
    mylog::GetLogger("asynclogger")->Info("RecycleClean() - Recycle bin cleaned successfully");
}
};