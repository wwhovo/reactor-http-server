#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define BUFFER_DEFAULT_SIZE 1024
class Buffer {
    private:
        std::vector<char> _buffer; //使用vector进行内存空间管理
        uint64_t _reader_idx; //读偏移
        uint64_t _writer_idx; //写偏移
    public:
        Buffer();
        char *Begin();
        //获取当前写入起始地址, _buffer的空间起始地址，加上写偏移量
        char *WritePosition();
        //获取当前读取起始地址
        char *ReadPosition();
        //获取缓冲区末尾空闲空间大小--写偏移之后的空闲空间, 总体空间大小减去写偏移
        uint64_t TailIdleSize();
        //获取缓冲区起始空闲空间大小--读偏移之前的空闲空间
        uint64_t HeadIdleSize();
        //获取可读数据大小 = 写偏移 - 读偏移
        uint64_t ReadAbleSize();
        //将读偏移向后移动
        void MoveReadOffset(uint64_t len);
        //将写偏移向后移动 
        void MoveWriteOffset(uint64_t len);
        //确保可写空间足够（整体空闲空间够了就移动数据，否则就扩容）
        void EnsureWriteSpace(uint64_t len); 
        //写入数据
        void Write(const void *data, uint64_t len);
        void WriteAndPush(const void *data, uint64_t len);
        void WriteString(const std::string &data);
        void WriteStringAndPush(const std::string &data);
        void WriteBuffer(Buffer &data);
        void WriteBufferAndPush(Buffer &data);
        //读取数据
        void Read(void *buf, uint64_t len);
        void ReadAndPop(void *buf, uint64_t len);
        std::string ReadAsString(uint64_t len);
        std::string ReadAsStringAndPop(uint64_t len);
        char *FindCRLF();
        /*通常获取一行数据，这种情况针对是*/
        std::string GetLine();
        std::string GetLineAndPop();
        //清空缓冲区
        void Clear();
};
