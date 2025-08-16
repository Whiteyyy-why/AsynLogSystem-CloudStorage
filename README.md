## 项目概述

本项目基于**libevent**网络库实现文件上传服务，支持上传、下载、删除和展示功能，支持low/deep（不压缩/压缩）存储等级的存储服务，新实现了回收站页面，主页面删除为软删除，回收站支持软删除的文件的恢复，彻底删除等功能；
携带了异步日志系统，支持备份重要日志，多线程并发写日志等功能。

## 环境准备

Ubuntu22.04 LTS

g++安装

```bash
sudo update
sudo apt install g++
```

### 日志部分

#### 1. jsoncpp

```bash
sudo apt-get libjsoncpp-dev
其头文件所在路径是：/usr/include/jsoncpp/json
动态库在：/usr/lib/x86_64-linux-gnu/libjsoncpp.so-版本号
编译时需要根据动态库的路径进行正确的设置，否则很容易出现“undefined reference to”问题。
使用g++编译时直接加上“-ljsoncpp”选项即可。
```

### 存储部分

### 服务端

#### 1. libevent

Linux下安装方法：

```bash
sudo apt-get update
sudo apt-get install build-essential autoconf automake
sudo apt-get install libssl-dev
./configure
wget https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz
tar xvf libevent-2.1.12-stable.tar.gz  //解压下载的源码压缩包，目录下会生成一个libevent-2.1.12-stable目录
cd libevent-2.1.12-stable                 //切换到libevent-2.1.12-stable目录,(安装步骤可以查看README.md文件)
./configure                                     //生成Makefile文件，用ll Makefile可以看到Makefile文件已生成
make                                          //编译
sudo make install                            //安装

# 最后检测是否成功安装
cd sample     //切换到sample目录
./hello-world   //运行hello-world可执行文件
# 新建一个终端，输入以下代码
nc 127.1 9995 //若安装成功，该终端会返回一个Hello, World!
```

#### 2. jsoncpp

日志部分已经安装

#### 3. bundle

源码链接：https://github.com/r-lyeh-archived/bundle

克隆下来包含bundle.cpp与bundle.h即可使用 
#### 4. cpp-base64
`git clone https://github.com/ReneNyffenegger/cpp-base64.git`
之后把该目录内的base64.h和.cpp文件拷贝到本项目文件src/server/下即可使用

### web端
ip+port即可访问

## 运行方式
先把AsynLogSystem-CloudStorage/src/server目录下的Storage.conf文件中的下面两个字段配好，如下面，替换成你自己的服务器ip地址和要使用的端口号，（如果使用的是云服务器需要去你买的云服务器示例下开放安全组规则允许外界访问该端口）。
```
"server_port" : 8081,
"server_ip" : "127.0.0.1"
```
再把AsynLogSystem-CloudStorage/log_system/logs_code下的config.conf文件中的如下两个字段配好，这两个字段是备份日志存放的服务器地址和端口号。（这个配置是可选的，如果没有配置，会链接错误，备份日志功能不会被启动，但是不影响其他部分日志系统的功能，本机还是可以正常写日志的）
```
"backup_addr" : "47.116.22.222",
"backup_port" : 8080
```
把log_stsytem目录下的backlog目录中的ServerBackupLog.cpp和hpp文件拷贝置另外一个服务器或当前服务器作为备份日志服务器，使用命令`g++ ServerBackupLog.cpp`生成可执行文件，`./a.out 端口号` 即可启动备份日志服务器，这里端口号由输入的端口号决定，要与客户端config.conf里的backup_port字段保持一致。

在Kama-AsynLogSystem-CloudStorage/src/server目录下使用make命令，生成test可执行文件，./test就可以运行起来了。
打开浏览器输入ip+port即可访问该服务，
或按照上方可选客户端实现，启动客户端后添加文件到对应文件夹即可上传文件
