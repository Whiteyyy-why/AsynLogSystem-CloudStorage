// 远程备份debug等级以上的日志信息-发送端
#include <iostream> // 标准输入输出
#include <cstring> // 用于memset
#include <string> // 字符串操作
#include <sys/types.h> // 套接字类型
#include <sys/socket.h> // 套接字函数
#include <sys/stat.h> // 文件状态，尽管在此文件中未直接使用stat
#include <arpa/inet.h> // 用于inet_aton
#include <netinet/in.h> // 用于sockaddr_in
#include <unistd.h> // 用于close
#include "../Util.hpp" // 包含mylog::Util::JsonData，用于获取配置信息

// 声明外部全局变量，用于访问日志配置数据（特别是备份服务器的地址和端口）
extern mylog::Util::JsonData* g_conf_data;

// start_backup函数：负责创建TCP客户端套接字并发送日志消息到备份服务器
void start_backup(const std::string& message)
{
    // 1. 创建套接字
    // AF_INET: IPv4协议族
    // SOCK_STREAM: TCP流式套接字
    // 0: 默认协议 (TCP)
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) // 检查套接字创建是否成功
    {
        std::cout << __FILE__ << __LINE__ << "socket error : " << strerror(errno) << std::endl;
        perror(NULL);
        return;
    }

    struct sockaddr_in server; // 定义服务器地址结构体
    memset(&server, 0, sizeof(server)); // 清零
    server.sin_family = AF_INET; // 地址族设置为IPv4
    server.sin_port = htons(g_conf_data->backup_port); // 从全局配置获取备份端口并转换为网络字节序
    // 将备份服务器IP地址字符串转换为网络字节序的二进制形式
    inet_aton(g_conf_data->backup_addr.c_str(), &(server.sin_addr));

    // 2. 尝试连接服务器，并实现重连机制
    int cnt = 5; // 重试连接次数
    while (-1 == connect(sock, (struct sockaddr*)&server, sizeof(server))) // 尝试建立TCP连接
    {
        std::cout << "正在尝试重连,重连次数还有: " << cnt-- << std::endl;
        if (cnt <= 0) // 重试次数耗尽，连接失败
        {
            std::cout << __FILE__ << __LINE__ << "connect error : " << strerror(errno) << std::endl;
            close(sock); // 关闭套接字
            perror(NULL);
            return; // 退出函数
        }
    }

    // 3. 连接成功，发送数据
    // char buffer[1024]; // 此缓冲区在此处未被使用，可能为残留代码
    if (-1 == write(sock, message.c_str(), message.size())) // 通过套接字发送日志消息
    {
        std::cout << __FILE__ << __LINE__ << "send to server error : " << strerror(errno) << std::endl;
        perror(NULL);
    }
    close(sock); // 发送完毕后关闭套接字
}