#pragma once
#include <iostream> // 用于标准输入输出
#include <string> // 用于字符串操作
#include <unistd.h> // 用于close函数
#include <cstring> // 用于memset
#include <cerrno> // 用于errno
#include <cstdlib> // 用于exit函数
#include <pthread.h> // 用于多线程编程
#include <mutex> // 用于互斥锁，尽管此处未直接使用std::mutex，但pthread_mutex_t是其底层实现
#include <sys/socket.h> // 用于套接字编程
#include <sys/types.h> // 用于socket类型
#include <netinet/in.h> // 用于sockaddr_in结构
#include <arpa/inet.h> // 用于inet_ntoa函数
#include <functional> // 用于std::function

using std::cout;
using std::endl;

using func_t = std::function<void(const std::string&)>; // 定义回调函数类型，用于处理接收到的数据
const int backlog = 32; // 定义listen函数的backlog参数，表示等待连接队列的最大长度

class TcpServer; // 前向声明TcpServer类

// ThreadData类：包含传递给新线程的客户端连接信息和TcpServer实例指针
class ThreadData
{
public:
    // 构造函数
    ThreadData(int fd, const std::string& ip, const uint16_t& port, TcpServer* ts)
        : sock(fd), client_ip(ip), client_port(port), ts_(ts) {
    }

public:
    int sock; // 客户端连接套接字文件描述符
    std::string client_ip; // 客户端IP地址
    uint16_t client_port; // 客户端端口号
    TcpServer* ts_; // 指向TcpServer实例的指针，以便线程可以调用其成员方法
};

// TcpServer类：实现一个简单的多线程TCP服务器
class TcpServer
{
public:
    // 构造函数：初始化服务器监听端口和数据处理回调函数
    TcpServer(uint16_t port, func_t func)
        : port_(port), func_(func)
    {
    }

    // init_service方法：初始化服务器套接字
    void init_service()
    {
        // 创建监听套接字 (AF_INET: IPv4, SOCK_STREAM: TCP流式套接字, 0: 默认协议)
        listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock_ == -1) {
            std::cout << __FILE__ << __LINE__ << "create socket error" << strerror(errno) << std::endl;
        }

        struct sockaddr_in local; // 定义服务器地址结构
        memset(&local, 0, sizeof(local)); // 清零
        local.sin_family = AF_INET; // 地址族IPv4
        local.sin_port = htons(port_); // 端口号转换为网络字节序
        local.sin_addr.s_addr = htonl(INADDR_ANY); // IP地址设置为任意可用IP，并转换为网络字节序

        // 绑定套接字到本地地址和端口
        if (bind(listen_sock_, (struct sockaddr*)&local, sizeof(local)) < 0) {
            std::cout << __FILE__ << __LINE__ << "bind socket error" << strerror(errno) << std::endl;
        }

        // 启动监听，将套接字设置为被动模式，等待连接
        if (listen(listen_sock_, backlog) < 0) {
            std::cout << __FILE__ << __LINE__ << "listen error" << strerror(errno) << std::endl;
        }
    }

    // threadRoutine方法：新创建线程的入口点 (静态方法，符合pthread_create要求)
    static void* threadRoutine(void* args)
    {
        pthread_detach(pthread_self()); // 将线程设置为分离状态，使其资源在结束后自动释放，防止在主线程阻塞等待

        ThreadData* td = static_cast<ThreadData*>(args); // 将void*参数转换为ThreadData指针
        std::string client_info = td->client_ip + ":" + std::to_string(td->client_port); // 格式化客户端IP和端口信息
        td->ts_->service(td->sock, move(client_info)); // 调用TcpServer实例的service方法处理连接
        close(td->sock); // 关闭客户端连接套接字
        delete td; // 释放ThreadData内存
        return nullptr;
    }

    // start_service方法：服务器主循环，接受并分发连接
    void start_service()
    {
        while (true) // 无限循环，持续接受连接
        {
            struct sockaddr_in client_addr; // 用于存储客户端地址信息
            socklen_t client_addrlen = sizeof(client_addr);
            // 接受客户端连接，返回一个新的套接字文件描述符connfd
            int connfd = accept(listen_sock_, (struct sockaddr*)&client_addr, &client_addrlen);
            if (connfd < 0) {
                std::cout << __FILE__ << __LINE__ << "accept error" << strerror(errno) << std::endl;
                continue; // 接受失败，继续下一次循环
            }

            // 获取客户端IP和端口信息
            std::string client_ip = inet_ntoa(client_addr.sin_addr); // 将网络字节序IP转换为字符串
            uint16_t client_port = ntohs(client_addr.sin_port); // 将网络字节序端口转换为主机字节序

            // 为每个客户端连接创建一个新线程来提供服务
			pthread_t tid; // 线程ID pthread_t 是 POSIX 线程（pthread）库中用于表示线程ID的数据类型
            ThreadData* td = new ThreadData(connfd, client_ip, client_port, this); // 创建ThreadData对象传递给线程
            pthread_create(&tid, nullptr, threadRoutine, td); // 创建线程
        }
    }

    // service方法：处理单个客户端连接的实际逻辑 (由工作线程调用)
    void service(int sock, const std::string&& client_info)
    {
        char buf[1024]; // 缓冲区

        int r_ret = read(sock, buf, sizeof(buf)); // 从客户端套接字读取数据
        if (r_ret == -1) {
            std::cout << __FILE__ << __LINE__ << "read error" << strerror(errno) << std::endl;
            perror("NULL");
        }
        else if (r_ret > 0) { // 成功读取到数据
            buf[r_ret] = 0; // 将读取到的数据以null终止，使其成为C字符串
            std::string tmp = buf;
            func_(client_info + tmp); // 调用回调函数处理接收到的数据，通常是写入日志文件
        }
    }
    // 默认析构函数，这里没有显式定义，但如果需要清理资源应显式定义
    ~TcpServer() = default;

private:
    int listen_sock_; // 服务器监听套接字
    uint16_t port_; // 服务器监听端口
    func_t func_; // 处理接收到数据的回调函数
};