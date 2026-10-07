#include "Buffer.hpp"
#include "../base/Logging.hpp"

Buffer::Buffer():_reader_idx(0), _writer_idx(0), _buffer(BUFFER_DEFAULT_SIZE) {}

char *Buffer::Begin() { return &*_buffer.begin(); }

char *Buffer::WritePosition() { return Begin() + _writer_idx; }

char *Buffer::ReadPosition() { return Begin() + _reader_idx; }

uint64_t Buffer::TailIdleSize() { return _buffer.size() - _writer_idx; }

uint64_t Buffer::HeadIdleSize() { return _reader_idx; }

uint64_t Buffer::ReadAbleSize() { return _writer_idx - _reader_idx; }

void Buffer::MoveReadOffset(uint64_t len) { 
    if (len == 0) return; 
    //向后移动的大小，必须小于可读数据大小
    assert(len <= ReadAbleSize());
    _reader_idx += len;
    if (_reader_idx == _writer_idx) {
        _reader_idx = _writer_idx = 0;
        if (_buffer.size() > 1024 * 1024) std::vector<char>(BUFFER_DEFAULT_SIZE).swap(_buffer);
    }
}

void Buffer::MoveWriteOffset(uint64_t len) {
    //向后移动的大小，必须小于当前后边的空闲空间大小
    assert(len <= TailIdleSize());
    _writer_idx += len;
}

void Buffer::EnsureWriteSpace(uint64_t len) {
    //如果末尾空闲空间大小足够，直接返回
    if (TailIdleSize() >= len) { return; }
    //末尾空闲空间不够，则判断加上起始位置的空闲空间大小是否足够, 够了就将数据移动到起始位置
    if (len <= TailIdleSize() + HeadIdleSize()) {
        //将数据移动到起始位置
        uint64_t rsz = ReadAbleSize();//把当前数据大小先保存起来
        std::copy(ReadPosition(), ReadPosition() + rsz, Begin());//把可读数据拷贝到起始位置
        _reader_idx = 0;    //将读偏移归0
        _writer_idx = rsz;  //将写位置置为可读数据大小， 因为当前的可读数据大小就是写偏移量
    }else {
        //总体空间不够，则需要扩容，不移动数据，直接给写偏移之后扩容足够空间即可
        DBG_LOG("RESIZE %ld", _writer_idx + len);
        _buffer.resize(_writer_idx + len);
    }
}

void Buffer::Write(const void *data, uint64_t len) {
    //1. 保证有足够空间，2. 拷贝数据进去
    if (len == 0) return;
    EnsureWriteSpace(len);
    const char *d = (const char *)data;
    std::copy(d, d + len, WritePosition());
}

void Buffer::WriteAndPush(const void *data, uint64_t len) {
    Write(data, len);
    MoveWriteOffset(len);
}

void Buffer::WriteString(const std::string &data) {
    return Write(data.c_str(), data.size());
}

void Buffer::WriteStringAndPush(const std::string &data) {
    WriteString(data);
    MoveWriteOffset(data.size());
}

void Buffer::WriteBuffer(Buffer &data) {
    return Write(data.ReadPosition(), data.ReadAbleSize());
}

void Buffer::WriteBufferAndPush(Buffer &data) { 
    WriteBuffer(data);
    MoveWriteOffset(data.ReadAbleSize());
}

void Buffer::Read(void *buf, uint64_t len) {
    //要求要获取的数据大小必须小于可读数据大小
    assert(len <= ReadAbleSize());
    std::copy(ReadPosition(), ReadPosition() + len, (char*)buf);
}

void Buffer::ReadAndPop(void *buf, uint64_t len) {
    Read(buf, len);
    MoveReadOffset(len);
}

std::string Buffer::ReadAsString(uint64_t len) {
    //要求要获取的数据大小必须小于可读数据大小
    assert(len <= ReadAbleSize());
    std::string str;
    str.resize(len);
    Read(&str[0], len);
    return str;
}

std::string Buffer::ReadAsStringAndPop(uint64_t len) {
    assert(len <= ReadAbleSize());
    std::string str = ReadAsString(len);
    MoveReadOffset(len);
    return str;
}

char *Buffer::FindCRLF() {
    char *res = (char*)memchr(ReadPosition(), '\n', ReadAbleSize());
    return res;
}

std::string Buffer::GetLine() {
    char *pos = FindCRLF();
    if (pos == NULL) {
        return "";
    }
    // +1是为了把换行字符也取出来。
    return ReadAsString(pos - ReadPosition() + 1);
}

std::string Buffer::GetLineAndPop() {
    std::string str = GetLine();
    MoveReadOffset(str.size());
    return str;
}

void Buffer::Clear() {
    //只需要将偏移量归0即可
    _reader_idx = 0;
    _writer_idx = 0;
}
