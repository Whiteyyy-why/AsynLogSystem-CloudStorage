// 远程备份debug等级以上的日志信息-接收端
#include <string> // 字符串操作
#include <cassert> // 断言
#include <cstring> // 字符串操作
#include <cstdlib> // 用于atoi, exit
#include <iostream> // 标准输入输出
#include <unistd.h> // 用于文件操作，如close
#include <memory> // 用于智能指针std::unique_ptr
#include <sys/stat.h> // 用于文件状态检查stat
#include <sys/types.h> // 用于stat类型
#include <sys/socket.h> // 用于套接字编程
#include <netinet/in.h> // 用于网络地址结构
#include "ServerBackupLog.hpp" // 包含TcpServer和ThreadData定义

using std::cout;
using std::endl;

const std::string filename = "./logfile.log"; // 定义日志备份文件的名称

// usage函数：打印程序使用说明
void usage(std::string procgress)
{
    cout << "usage error:" << procgress << "port" << endl;
}

// file_exist函数：检查文件是否存在（此处未被main函数直接使用）
bool file_exist(const std::string& name)
{
    struct stat exist;
    return (stat(name.c_str(), &exist) == 0);
}

// backup_log函数：用作TcpServer的回调函数，将接收到的日志消息写入文件
void backup_log(const std::string& message) // 用作回调
{
    // 以追加二进制模式打开日志文件
    FILE* fp = fopen(filename.c_str(), "ab");
    if (fp == NULL) // 检查文件是否成功打开
    {
        perror("fopen error: ");
        assert(false); // 如果文件无法打开，触发断言
    }
    // 将接收到的日志消息写入文件
    int write_byte = fwrite(message.c_str(), 1, message.size(), fp);
    if (write_byte != message.size()) // 检查是否所有字节都成功写入
    {
        perror("fwrite error: ");
        assert(false); // 如果写入失败，触发断言
    }
    fflush(fp); // 刷新文件缓冲区到操作系统
    fclose(fp); // 关闭文件
}

// main函数：日志备份服务器的程序入口
int main(int args, char* argv[])
{
    // 检查命令行参数数量，需要一个端口号参数
    if (args != 2)
    {
        usage(argv[0]); // 打印使用说明
        perror("usage error");
        exit(-1); // 退出程序
    }

    uint16_t port = atoi(argv[1]); // 从命令行参数获取端口号并转换为整数
    // 创建TcpServer实例，传入端口号和backup_log作为回调函数
    std::unique_ptr<TcpServer> tcp(new TcpServer(port, backup_log));

    tcp->init_service(); // 初始化TCP服务器的套接字
    tcp->start_service(); // 启动TCP服务器的主循环，开始接受连接

    return 0; // 程序正常退出
}