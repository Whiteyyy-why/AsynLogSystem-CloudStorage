#include <cassert> // 用于断言
#include <fstream> // 用于文件流操作
#include <memory> // 用于智能指针
#include <unistd.h> // 用于fsync函数
#include "Util.hpp" // 包含mylog::Util::File和mylog::Util::Date，以及mylog::Util::JsonData

// 声明外部全局变量，用于访问日志配置数据
extern mylog::Util::JsonData* g_conf_data;

namespace mylog {
    // LogFlush是所有日志刷新器的抽象基类
    class LogFlush
    {
    public:
        using ptr = std::shared_ptr<LogFlush>; // 定义智能指针类型
        virtual ~LogFlush() {} // 虚析构函数，确保正确释放派生类资源
        // 纯虚函数：不同的写文件方式（如stdout, 文件, 滚动文件）需要实现自己的Flush逻辑
        virtual void Flush(const char* data, size_t len) = 0;
    };

    // StdoutFlush是LogFlush的派生类，将日志刷新到标准输出
    class StdoutFlush : public LogFlush
    {
    public:
        using ptr = std::shared_ptr<StdoutFlush>; // 定义智能指针类型
        // 实现Flush方法，将数据写入到cout (标准输出流)
        void Flush(const char* data, size_t len) override {
            std::cout.write(data, len);
        }
    };

    // FileFlush是LogFlush的派生类，将日志刷新到指定文件
    class FileFlush : public LogFlush
    {
    public:
        using ptr = std::shared_ptr<FileFlush>; // 定义智能指针类型
        // 构造函数：接收文件名，并创建目录、打开文件
        FileFlush(const std::string& filename) : filename_(filename)
        {
            // 创建日志文件所在的目录
            mylog::Util::File::CreateDirectory(mylog::Util::File::Path(filename));
            // 打开文件，以追加二进制模式 ('ab')
            fs_ = fopen(filename.c_str(), "ab");
            if (fs_ == NULL) {
                std::cout << __FILE__ << __LINE__ << "open log file failed" << std::endl;
                perror(NULL);
            }
        }
        // 实现Flush方法，将数据写入文件
        void Flush(const char* data, size_t len) override {
            fwrite(data, 1, len, fs_); // 将数据写入文件
            if (ferror(fs_)) { // 检查写入错误
                std::cout << __FILE__ << __LINE__ << "write log file failed" << std::endl;
                perror(NULL);
            }
            // 根据配置文件中的flush_log设置，决定何时同步到磁盘
            if (g_conf_data->flush_log == 1) { // 立即执行fflush (缓冲区刷新到OS缓存)
                if (fflush(fs_) == EOF) {
                    std::cout << __FILE__ << __LINE__ << "fflush file failed" << std::endl;
                    perror(NULL);
                }
            }
            else if (g_conf_data->flush_log == 2) { // 执行fflush和fsync (OS缓存刷新到物理磁盘)
                fflush(fs_);
                fsync(fileno(fs_));
            }
        }

    private:
        std::string filename_; // 日志文件名
        FILE* fs_ = NULL; // 文件指针
    };

    // RollFileFlush是LogFlush的派生类，实现日志文件滚动功能
    class RollFileFlush : public LogFlush
    {
    public:
        using ptr = std::shared_ptr<RollFileFlush>; // 定义智能指针类型
        // 构造函数：接收文件名基础和最大文件大小
        RollFileFlush(const std::string& filename, size_t max_size)
            : max_size_(max_size), basename_(filename)
        {
            // 创建日志文件所在的目录
            mylog::Util::File::CreateDirectory(mylog::Util::File::Path(filename));
        }

        // 实现Flush方法，将数据写入文件，并在文件大小达到阈值时进行滚动
        void Flush(const char* data, size_t len) override
        {
            InitLogFile(); // 检查是否需要滚动，并确保文件打开
            // 向当前日志文件写入内容
            fwrite(data, 1, len, fs_);
			if (ferror(fs_)) { // 检查写入错误
                std::cout << __FILE__ << __LINE__ << "write log file failed" << std::endl;
                perror(NULL);
            }
            cur_size_ += len; // 更新当前文件大小
            // 根据配置决定刷新到磁盘的时机，与FileFlush相同
            if (g_conf_data->flush_log == 1) {
                if (fflush(fs_)) {
                    std::cout << __FILE__ << __LINE__ << "fflush file failed" << std::endl;
                    perror(NULL);
                }
            }
            else if (g_conf_data->flush_log == 2) {
                fflush(fs_);
                fsync(fileno(fs_));
            }
        }

    private:
        // 初始化或滚动日志文件
        void InitLogFile()
        {
            // 如果文件未打开或当前文件大小超过最大限制，则需要滚动（开始写新的文件）
            if (fs_ == NULL || cur_size_ >= max_size_)
            {
                if (fs_ != NULL) {
                    fclose(fs_); // 关闭当前文件（如果已打开）
                    fs_ = NULL;
                }
                std::string filename = CreateFilename(); // 创建新的文件名
                fs_ = fopen(filename.c_str(), "ab"); // 打开新文件
                if (fs_ == NULL) {
                    std::cout << __FILE__ << __LINE__ << "open file failed" << std::endl;
                    perror(NULL);
                }
                cur_size_ = 0; // 重置当前文件大小
            }
        }

        // 构建滚动日志文件名称（包含时间戳和计数器）
        std::string CreateFilename()
        {
            time_t time_ = mylog::Util::Date::Now(); // 获取当前时间戳
            struct tm t;
            localtime_r(&time_, &t); // 转换为本地时间
            std::string filename = basename_; // 使用基础文件名
            // 拼接年月日时分秒和计数器
            filename += std::to_string(t.tm_year + 1900);
            filename += std::to_string(t.tm_mon + 1);
            filename += std::to_string(t.tm_mday);
            filename += std::to_string(t.tm_hour + 1);
            filename += std::to_string(t.tm_min + 1);
            filename += std::to_string(t.tm_sec + 1) + '-' +
                std::to_string(cnt_++) + ".log"; // 拼接计数器和.log后缀
			return filename; // 完整文件名 示例：20250325123456-1.log
        }

    private:
        size_t cnt_ = 1; // 滚动文件计数器
        size_t cur_size_ = 0; // 当前文件大小
        size_t max_size_; // 最大文件大小，超过则滚动
        std::string basename_; // 日志文件名的基础部分
        FILE* fs_ = NULL; // 文件指针
    };

    // LogFlushFactory提供静态方法来创建不同类型的LogFlush实例
    class LogFlushFactory
    {
    public:
        using ptr = std::shared_ptr<LogFlushFactory>; // 定义智能指针类型
        // 模板方法，用于创建指定FlushType的LogFlush实例
        // Args...是可变参数模板，用于传递给FlushType构造函数
        template <typename FlushType, typename... Args>
        static std::shared_ptr<LogFlush> CreateLog(Args &&...args)
        {
            // 使用std::make_shared创建FlushType的智能指针实例
            return std::make_shared<FlushType>(std::forward<Args>(args)...);
        }
    };
	// LogFlushFactory创造实例
	// 创建FileFlush实例，传入日志文件名
	// LogFlushFactory::CreateLog<RollFileFlush>("log.txt", 1024 * 1024) 创建RollFileFlush实例，传入日志文件名和最大文件大小
	//// 创建StdoutFlush实例
	//static std::shared_ptr<LogFlush> CreateStdoutFlush()
	//{
	//	return LogFlushFactory::CreateLog<StdoutFlush>();
	//}
	//// 创建FileFlush实例
	//static std::shared_ptr<LogFlush> CreateFileFlush(const std::string& filename)
	//{
	//	return LogFlushFactory::CreateLog<FileFlush>(filename);
	//}
	//// 创建RollFileFlush实例
	//static std::shared_ptr<LogFlush> CreateRollFileFlush(const std::string& filename, size_t max_size)
	//{
	//	return LogFlushFactory::CreateLog<RollFileFlush>(filename, max_size);
	//}
} // namespace mylog