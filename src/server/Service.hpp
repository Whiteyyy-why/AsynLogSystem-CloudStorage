#pragma once
#include "DataManager.hpp" // 包含DataManager和StorageInfo，用于管理文件元数据

#include <sys/queue.h> // libevent内部可能使用，通常不直接用到
#include <event.h> // libevent核心头文件
// for http
#include <evhttp.h> // libevent的HTTP模块
#include <event2/http.h> // libevent的HTTP模块 (新版本路径)

#include <fcntl.h> // 文件控制，如open函数
#include <sys/stat.h> // 文件状态，如open函数

#include <regex> // 正则表达式，用于HTML模板替换

#include "base64.h" // 来自 cpp-base64 库，用于文件名编码/解码

// 声明外部全局的DataManager指针
extern storage::DataManager* data_;

namespace storage
{
    // Service类：实现HTTP服务器的主要逻辑
    class Service
    {
    public:
        // 构造函数：读取服务器配置
        Service()
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

        // RunModule方法：启动HTTP服务器
        bool RunModule()
        {
            // 初始化libevent事件基础
            event_base* base = event_base_new();
            if (base == NULL)
            {
                mylog::GetLogger("asynclogger")->Fatal("event_base_new err!"); // 记录致命错误
                return false;
            }

            // 设置监听的端口和地址 (sockaddr_in结构体在此处虽然定义但未直接用于绑定，evhttp_bind_socket会处理)
            sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(server_port_);

            // 创建evhttp上下文，用于处理HTTP请求
            evhttp* httpd = evhttp_new(base);

            // 绑定HTTP服务器到所有可用IP地址和指定端口
            if (evhttp_bind_socket(httpd, "0.0.0.0", server_port_) != 0)
            {
                mylog::GetLogger("asynclogger")->Fatal("evhttp_bind_socket failed!"); // 记录致命错误
                return false;
            }

            // 设定通用的HTTP请求回调函数
            evhttp_set_gencb(httpd, GenHandler, NULL);

            if (base)
            {
#ifdef DEBUG_LOG
                mylog::GetLogger("asynclogger")->Debug("event_base_dispatch"); // 记录调试日志
#endif
                // 启动libevent事件循环，开始监听和处理HTTP请求
                if (-1 == event_base_dispatch(base))
                {
                    mylog::GetLogger("asynclogger")->Debug("event_base_dispatch err"); // 记录调试错误
                }
            }
            // 清理libevent资源
            if (base)
                event_base_free(base);
            if (httpd)
                evhttp_free(httpd);
            return true;
        }

    private:
        uint16_t server_port_; // 服务器监听端口
        std::string server_ip_; // 服务器IP地址
        std::string download_prefix_; // 下载URL前缀

    private:
        // GenHandler：通用的HTTP请求分发器 (静态回调函数)
        static void GenHandler(struct evhttp_request* req, void* arg)
        {
            // 获取请求URI路径并进行URL解码
            std::string path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));
            path = UrlDecode(path); // 使用自定义的UrlDecode函数
            mylog::GetLogger("asynclogger")->Info("get req, uri: %s", path.c_str()); // 记录请求URI

            // 根据请求路径判断请求类型并分发
            if (path.find("/download/") != std::string::npos) // 下载请求
            {
                Download(req, arg);
            }
            else if (path == "/upload") // 上传请求
            {
                Upload(req, arg);
            }
            else if(path == "/delete") // 删除请求
            {
                Delete(req, arg);
			}
            else if (path == "/") // 显示文件列表请求
            {
                ListShow(req, arg);
            }
            else // 未知请求，返回404
            {
                evhttp_send_reply(req, HTTP_NOTFOUND, "Not Found", NULL);
            }
        }

        // Upload：处理文件上传请求
        static void Upload(struct evhttp_request* req, void* arg)
        {
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
                evhttp_send_reply(req, HTTP_BADREQUEST, "file empty", NULL);
                mylog::GetLogger("asynclogger")->Info("request body is empty");
                return;
            }
            std::string content(len, 0); // 创建字符串存储请求体内容
            if (-1 == evbuffer_copyout(buf, (void*)content.c_str(), len)) // 将缓冲区内容复制到字符串
            {
                mylog::GetLogger("asynclogger")->Error("evbuffer_copyout error");
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
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: HTTP_BADREQUEST");
                evhttp_send_reply(req, HTTP_BADREQUEST, "Illegal storage type", NULL);
                return;
            }

            // 如果存储目录不存在，则创建
            FileUtil dirCreate(storage_path_dir);
            dirCreate.CreateDirectory();

            // 完整的最终文件存储路径
            std::string final_storage_path = storage_path_dir + filename;
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Debug("storage_path:%s", final_storage_path.c_str());
#endif

            // 根据存储类型写入文件 (low_storage直接写入，deep_storage压缩后写入)
            FileUtil fu(final_storage_path);
            if (final_storage_path.find("low_storage") != std::string::npos) // 普通存储
            {
                if (fu.SetContent(content.c_str(), len) == false) // 直接写入内容
                {
                    mylog::GetLogger("asynclogger")->Error("low_storage fail, evhttp_send_reply: HTTP_INTERNAL");
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
                    mylog::GetLogger("asynclogger")->Error("deep_storage fail, evhttp_send_reply: HTTP_INTERNAL");
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

            evhttp_send_reply(req, HTTP_OK, "Success", NULL); // 返回成功响应
            mylog::GetLogger("asynclogger")->Info("upload finish:success");
        }

        // TimetoStr：将time_t时间转换为字符串 (此处仅为辅助，实际在ListShow中被generateModernFileList调用)
        static std::string TimetoStr(time_t t)
        {
            std::string tmp = std::ctime(&t); // 使用ctime将时间戳转为可读字符串
            return tmp;
        }

        // generateModernFileList：生成HTML文件列表片段
        static std::string generateModernFileList(const std::vector<StorageInfo>& files)
        {
            std::stringstream ss;
            ss << "<div class='file-list'><h3>已上传文件</h3>";

            for (const auto& file : files) // 遍历所有StorageInfo
            {
                std::string filename = FileUtil(file.storage_path_).FileName(); // 获取文件名

                // 从存储路径判断存储类型
                std::string storage_type = "low";
                if (file.storage_path_.find("deep") != std::string::npos)
                {
                    storage_type = "deep";
                }

                // 构建每个文件项的HTML
                ss << "<div class='file-item'>"
                    << "<div class='file-info'>"
                    << "<span>📄" << filename << "</span>"
                    << "<span class='file-type'>"
                    << (storage_type == "deep" ? "深度存储" : "普通存储")
                    << "</span>"
                    << "<span>" << formatSize(file.fsize_) << "</span>" // 格式化文件大小
                    << "<span>" << TimetoStr(file.mtime_) << "</span>" // 格式化修改时间
                    << "</div>"
                    // 下载按钮，点击跳转到下载URL
                    << "<button onclick=\"window.location='" << file.url_ << "'\">⬇️ 下载</button>"
					<< "<button onclick=\"window.location='/delete?url=" << file.url_ << "'\">🗑️ 删除</button>"
                    << "</div>";
            }

            ss << "</div>"; // 关闭文件列表div
            return ss.str();
        }

        // formatSize：格式化文件大小为可读单位 (B, KB, MB, GB)
        static std::string formatSize(uint64_t bytes)
        {
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
        static void ListShow(struct evhttp_request* req, void* arg)
        {
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
                generateModernFileList(arry)); // 替换为生成的文件列表HTML
            // 替换服务器地址
            templateContent = std::regex_replace(templateContent,
                std::regex("\\{\\{BACKEND_URL\\}\\}"), // 查找{{BACKEND_URL}}
                "http://" + storage::Config::GetInstance()->GetServerIp() + ":" + std::to_string(storage::Config::GetInstance()->GetServerPort()));

            // 获取请求的输出缓冲区
            struct evbuffer* buf = evhttp_request_get_output_buffer(req);
            auto response_body = templateContent;
            // 将生成的HTML内容添加到输出缓冲区
            evbuffer_add(buf, (const void*)response_body.c_str(), response_body.size());
            evhttp_add_header(req->output_headers, "Content-Type", "text/html;charset=utf-8"); // 设置响应头
            evhttp_send_reply(req, HTTP_OK, NULL, NULL); // 发送HTTP OK响应
            mylog::GetLogger("asynclogger")->Info("ListShow() finish"); // 记录日志
        }

        // GetETag：根据文件信息生成ETag (用于缓存和断点续传)
        static std::string GetETag(const StorageInfo& info)
        {
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
        static void Download(struct evhttp_request* req, void* arg)
        {
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
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: 500 - UnCompress failed");
                evhttp_send_reply(req, HTTP_INTERNAL, NULL, NULL);
            }
            else if (fu_download.Exists() == false && info.storage_path_.find("low_storage") == std::string::npos)
            {
                // 如果是普通文件，且文件不存在，是客户端的请求错误
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: 400 - bad request,file not exists");
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
                evhttp_send_reply(req, 404, download_path.c_str(), NULL); // 返回404
                return;
            }
            evbuffer* outbuf = evhttp_request_get_output_buffer(req); // 获取响应输出缓冲区
            int fd = open(download_path.c_str(), O_RDONLY); // 打开文件以供读取
            if (fd == -1) // 检查文件是否成功打开
            {
                mylog::GetLogger("asynclogger")->Error("open file error: %s -- %s", download_path.c_str(), strerror(errno));
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
                evhttp_send_reply(req, HTTP_OK, "Success", NULL); // 返回200 OK
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: HTTP_OK");
            }
            else // 断点续传请求
            {
                evhttp_send_reply(req, 206, "breakpoint continuous transmission", NULL); // 返回206 Partial Content
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: 206");
            }

            // 清理：如果下载路径是临时解压文件，则删除它
            if (download_path != info.storage_path_)
            {
                remove(download_path.c_str()); // 删除文件
            }
        }

		// Delete：处理文件删除请求
        static void Delete(struct evhttp_request* req, void* arg) {
            mylog::GetLogger("asynclogger")->Info("Delete start");
            
            // 获取请求方法
            evhttp_cmd_type method = evhttp_request_get_command(req);
            
            std::string url_to_delete;
            
            if (method == EVHTTP_REQ_GET) {
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
                evhttp_clear_headers(&params);
                
            } else if (method == EVHTTP_REQ_POST) {
                // 处理POST请求 - 从请求体获取
                struct evbuffer* buf = evhttp_request_get_input_buffer(req);
                size_t len = evbuffer_get_length(buf);
                
                if (len > 0) {
                    std::string content(len, 0);
                    evbuffer_copyout(buf, (void*)content.c_str(), len);
                    
                    // 解析JSON或表单数据
                    // 这里根据你的具体格式来解析
                    Json::Value root;
                    if (JsonUtil::UnSerialize(content, &root)) {
                        url_to_delete = root["url"].asString();
                    }
                    mylog::GetLogger("asynclogger")->Info("Delete URL from POST body: %s", url_to_delete.c_str());
                }
            }
            
            // 验证URL参数
            if (url_to_delete.empty()) {
                mylog::GetLogger("asynclogger")->Error("Delete request missing url parameter");
                evhttp_send_reply(req, HTTP_BADREQUEST, "Missing url parameter", NULL);
                return;
            }
            
            mylog::GetLogger("asynclogger")->Info("Attempting to delete file with URL: %s", url_to_delete.c_str());
            
            // 从DataManager中获取StorageInfo
            StorageInfo info;
            if (!data_->GetOneByURL(url_to_delete, &info)) {
                mylog::GetLogger("asynclogger")->Error("File not found in DataManager: %s", url_to_delete.c_str());
                evhttp_send_reply(req, HTTP_NOTFOUND, "File not found", NULL);
                return;
            }
            
            // 删除物理文件
            if (remove(info.storage_path_.c_str()) != 0) {
                mylog::GetLogger("asynclogger")->Error("Failed to delete physical file: %s", info.storage_path_.c_str());
                evhttp_send_reply(req, HTTP_INTERNAL, "Failed to delete file", NULL);
                return;
            }
            
            // 从DataManager中删除记录
            if (!data_->Delete(url_to_delete)) {
                mylog::GetLogger("asynclogger")->Error("Failed to delete record from DataManager: %s", url_to_delete.c_str());
                evhttp_send_reply(req, HTTP_INTERNAL, "Failed to delete record", NULL);
                return;
            }
            
            // 返回成功页面或重定向到列表页
            evhttp_add_header(req->output_headers, "Location", "/");
            evhttp_send_reply(req, HTTP_MOVETEMP, "File deleted, redirecting...", NULL);
            mylog::GetLogger("asynclogger")->Info("File deleted successfully: %s", url_to_delete.c_str());
        }

    };
}