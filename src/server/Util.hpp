#pragma once
#include "jsoncpp/json/json.h"
#include <cassert>
#include <sstream>
#include <memory>
#include "bundle.h"
#include "Config.hpp"
#include <iostream>
#include <experimental/filesystem>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <fstream>
#include "../../log_system/logs_code/MyLog.hpp"

namespace storage
{
    namespace fs = std::experimental::filesystem;

    static unsigned char ToHex(unsigned char x)
	{ // 将数字转换为十六进制字符
		return x > 9 ? x + 55 : x + 48; // 大于9的数字转换为A-F，小于等于9的数字转换为0-9
    }

    static unsigned char FromHex(unsigned char x)
	{ // 将十六进制字符转换为数字
        unsigned char y;
        if (x >= 'A' && x <= 'Z') // 大写字母A-F
			y = x - 'A' + 10; // 将A-F转换为10-15
        else if (x >= 'a' && x <= 'z') // 小写字母a-f
			y = x - 'a' + 10; // 将a-f转换为10-15
        else if (x >= '0' && x <= '9') // 数字0-9
			y = x - '0'; // 将0-9转换为0-9
        else
            assert(0);
        return y;
    }
    static std::string UrlDecode(const std::string &str)
    {
        std::string strTemp = "";
        size_t length = str.length();
        for (size_t i = 0; i < length; i++)
        {
            // if (str[i] == '+')
            //     strTemp += ' ';
            if (str[i] == '%') // 如果是百分号编码
            {
				assert(i + 2 < length); // 确保后面还有两个字符
				unsigned char high = FromHex((unsigned char)str[++i]); // 将下一个字符转换为高位数字
				unsigned char low = FromHex((unsigned char)str[++i]); // 将下一个字符转换为低位数字
				strTemp += high * 16 + low; // 将高位和低位数字组合成一个字符
            }
            else
				strTemp += str[i]; // 直接添加非百分号编码的字符
        }
        return strTemp;
    }

    class FileUtil
    {
    private:
		std::string filename_; // 文件名

    public:
        FileUtil(const std::string &filename) : filename_(filename) {}

        ////////////////////////////////////////////
        // 文件操作
        //  获取文件大小
        int64_t FileSize()
        {
            struct stat s;
			auto ret = stat(filename_.c_str(), &s); // 获取文件状态信息
            if (ret == -1)
            {
				mylog::GetLogger("asynclogger")->Info("%s, Get file size failed: %s", filename_.c_str(), strerror(errno)); // 错误日志，内容格式：文件名,Get file size failed: 错误信息
                return -1;
            }
            return s.st_size;
        }
        // 获取文件最近访问时间
        time_t LastAccessTime()
        {
            struct stat s;
            auto ret = stat(filename_.c_str(), &s);
            if (ret == -1)
            {
                mylog::GetLogger("asynclogger")->Info("%s, Get file access time failed: %s", filename_.c_str(),strerror(errno));
                return -1;
            }
            return s.st_atime;
        }

        // 获取文件最近修改时间
        time_t LastModifyTime()
        {
            struct stat s;
            auto ret = stat(filename_.c_str(), &s);
            if (ret == -1)
            {
                mylog::GetLogger("asynclogger")->Info("%s, Get file modify time failed: %s",filename_.c_str(), strerror(errno));
                return -1;
            }
            return s.st_mtime;
        }

        // 从路径中解析出文件名
        std::string FileName()
        {
			auto pos = filename_.find_last_of("/"); // 查找最后一个斜杠的位置
			if (pos == std::string::npos) // 如果没有找到斜杠
				return filename_; // 返回整个路径作为文件名
			return filename_.substr(pos + 1, std::string::npos); // 返回斜杠后面的部分作为文件名 std::string::npos表示到字符串末尾
        }

        // 从文件POS处获取len长度字符给content
		bool GetPosLen(std::string* content, size_t pos, size_t len) // 获取文件指定位置和长度的内容
        {
            // 判断要求数据内容是否符合文件大小
            if (pos + len > FileSize())
            {
                mylog::GetLogger("asynclogger")->Info("needed data larger than file size");
                return false;
            }

            // 打开文件
            std::ifstream ifs;
			ifs.open(filename_.c_str(), std::ios::binary); // 以二进制模式打开文件
            if (ifs.is_open() == false)
            {
                mylog::GetLogger("asynclogger")->Info("%s,file open error",filename_.c_str());
                return false;
            }

            // 读入content
            ifs.seekg(pos, std::ios::beg); // 更改文件指针的偏移量
			content->resize(len); // 调整content的大小以容纳读取的数据
			ifs.read(&(*content)[0], len); // 从文件中读取指定长度的数据到content中
            if (!ifs.good())
            {
                mylog::GetLogger("asynclogger")->Info("%s,read file content error",filename_.c_str());
                ifs.close();
                return false;
            }
            ifs.close();

            return true;
        }

        // 获取文件内容
        bool GetContent(std::string *content)
        {
            return GetPosLen(content, 0, FileSize());
        }

        // 写文件
        bool SetContent(const char *content, size_t len)
        {
            std::ofstream ofs; 
            ofs.open(filename_.c_str(), std::ios::binary);
            if (!ofs.is_open())
            {
                mylog::GetLogger("asynclogger")->Info("%s open error: %s", filename_.c_str(), strerror(errno));
                return false;
            }
			ofs.write(content, len); // 将内容写入文件
            if (!ofs.good())
            {
                mylog::GetLogger("asynclogger")->Info("%s, file set content error",filename_.c_str());
                ofs.close();
            }
            ofs.close();
            return true;
        }

        //////////////////////////////////////////////
        // 压缩操作
        //  压缩文件
        bool Compress(const std::string &content, int format)
        {

			std::string packed = bundle::pack(format, content); // 对内容进行压缩 format指定压缩格式 如1代表gzip，2代表zlib等
			if (packed.size() == 0)// 如果压缩后的数据大小为0，说明压缩失败
            {
                mylog::GetLogger("asynclogger")->Info("Compress packed size error:%d", packed.size());
                return false;
            }
            // 将压缩的数据写入压缩包文件中
			FileUtil f(filename_); // 创建FileUtil对象，指定压缩包文件名
			if (f.SetContent(packed.c_str(), packed.size()) == false)// 如果写入压缩包文件失败
            {
                mylog::GetLogger("asynclogger")->Info("filename:%s, Compress SetContent error",filename_.c_str());
                return false;
            }
            return true;
        }
        bool UnCompress(std::string &download_path)
        {
            // 将当前压缩包数据读取出来
            std::string body;
			if (this->GetContent(&body) == false) // 如果获取压缩包内容失败
            {
                mylog::GetLogger("asynclogger")->Info("filename:%s, uncompress get file content failed!",filename_.c_str());
                return false;
            }
            // 对压缩的数据进行解压缩
            std::string unpacked = bundle::unpack(body);
            // 将解压缩的数据写入到新文件
			FileUtil fu(download_path); // 创建FileUtil对象，指定解压缩后的文件名
            if (fu.SetContent(unpacked.c_str(), unpacked.size()) == false)
            {
                mylog::GetLogger("asynclogger")->Info("filename:%s, uncompress write packed data failed!",filename_.c_str());
                return false;
            }
            return true;
        }
        ///////////////////////////////////////////
        // 目录操作
        // 以下三个函数使用c++17中文件系统给的库函数实现
        bool Exists()
        {
			return fs::exists(filename_); // fs是std::experimental::filesystem的别名，fs::exists检查文件或目录是否存在
        }

        bool CreateDirectory()
        {
            if (Exists())
                return true;
            return fs::create_directories(filename_);
        }

        bool ScanDirectory(std::vector<std::string> *arry) // 扫描目录下的所有文件
        {
            for (auto &p : fs::directory_iterator(filename_))
            {
                if (fs::is_directory(p) == true)
                    continue;
                // relative_path带有路径的文件名
				arry->push_back(fs::path(p).relative_path().string()); // 将相对路径转换为字符串并添加到数组中
            }
            return true;
        }
    };

    class JsonUtil
    {
    public:
        static bool Serialize(const Json::Value &val, std::string *str)
        {
            // 建造者生成->建造者实例化json写对象->调用写对象中的接口进行序列化写入str
            Json::StreamWriterBuilder swb;
            swb["emitUTF8"] = true;
            std::unique_ptr<Json::StreamWriter> usw(swb.newStreamWriter());
            std::stringstream ss;
            if (usw->write(val, &ss) != 0)
            {
                mylog::GetLogger("asynclogger")->Info("serialize error");
                return false;
            }
            *str = ss.str();
            return true;
        }
        static bool UnSerialize(const std::string &str, Json::Value *val)
        {
            // 操作方法类似序列化
            Json::CharReaderBuilder crb;
            std::unique_ptr<Json::CharReader> ucr(crb.newCharReader());
            std::string err;
            if (ucr->parse(str.c_str(), str.c_str() + str.size(), val, &err) == false)
            {
                mylog::GetLogger("asynclogger")->Info("parse error");
                return false;
            }
            return true;
        }
    };
}