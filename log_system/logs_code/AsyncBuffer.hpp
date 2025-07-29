/*日志缓冲区类设计*/
#pragma once
#include <cassert>
#include <string>
#include <vector>
#include "Util.hpp"

extern mylog::Util::JsonData* g_conf_data;
// 全局配置数据指针，指向JsonData单例对象
namespace mylog{
    class Buffer{
    public:
        Buffer() : write_pos_(0), read_pos_(0) {
			buffer_.resize(g_conf_data->buffer_size); // 初始化缓冲区大小
        }

        void Push(const char *data, size_t len)
        {
            ToBeEnough(len); // 确保容量足够
            // 开始写入
			std::copy(data, data + len, &buffer_[write_pos_]); // 将数据从data复制到缓冲区的写入位置
			write_pos_ += len; // 更新写入位置
        }
        char *ReadBegin(int len)
        {
			assert(len <= ReadableSize()); // 确保读取长度不超过可读数据大小
			return &buffer_[read_pos_]; // 返回可读数据的起始位置
        }
		bool IsEmpty() { return write_pos_ == read_pos_; }// 判断缓冲区是否为空

        void Swap(Buffer &buf)
        {
			buffer_.swap(buf.buffer_); // 交换缓冲区内容
			std::swap(read_pos_, buf.read_pos_); // 交换读位置
			std::swap(write_pos_, buf.write_pos_); // 交换写位置
        }
        size_t WriteableSize()
        { // 写空间剩余容量
            return buffer_.size() - write_pos_;
        }
        size_t ReadableSize()
        { // 读空间剩余容量
			return write_pos_ - read_pos_; // 计算可读数据大小
        }
		const char* Begin() { return &buffer_[read_pos_]; } // 获取可读数据的起始位置
        void MoveWritePos(int len)
        {
			assert(len <= WriteableSize()); // 确保写入长度不超过可写数据大小
			write_pos_ += len; // 更新写入位置
        }
        void MoveReadPos(int len)
        {
            assert(len <= ReadableSize());
            read_pos_ += len;
        }
        void Reset()
        { // 重置缓冲区
            write_pos_ = 0;
            read_pos_ = 0;
        }

    protected:
		void ToBeEnough(size_t len) // 确保缓冲区容量足够
        {
            int buffersize = buffer_.size();
			if (len >= WriteableSize()) // 如果需要写入的数据长度超过可写空间
            {
                if (buffer_.size() < g_conf_data->threshold) // 如果缓冲区小于阈值
                {
					buffer_.resize(2 * buffer_.size() + buffersize); // 扩大缓冲区大小
                }
                else
                {
					buffer_.resize(g_conf_data->linear_growth + buffersize); // 线性增长缓冲区大小
                }
            }
        }

    protected:
        std::vector<char> buffer_; // 缓冲区
        size_t write_pos_;         // 生产者此时的位置
        size_t read_pos_;          // 消费者此时的位置
    };
} // namespace mylog