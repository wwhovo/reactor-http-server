#pragma once
#include "../base/Logging.hpp"
#include "../base/NetworkInit.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_LISTEN 1024
class Socket {
    private:
        int _sockfd;
    public:
        Socket();
        Socket(int fd);
        ~Socket();
        Socket(const Socket &) = delete;
        Socket &operator=(const Socket &) = delete;
        int Fd();
        uint16_t LocalPort();
        //创建套接字
        bool Create();
        //绑定地址信息
        bool Bind(const std::string &ip, uint16_t port);
        //开始监听
        bool Listen(int backlog = MAX_LISTEN);
        //向服务器发起连接
        bool Connect(const std::string &ip, uint16_t port);
        
        //获取新连接
        int Accept();
        //接收数据
        ssize_t Recv(void *buf, size_t len, int flag = 0);
        ssize_t NonBlockRecv(void *buf, size_t len);
        //发送数据
        ssize_t Send(const void *buf, size_t len, int flag = 0);
        ssize_t NonBlockSend(void *buf, size_t len);
        //关闭套接字
        void Close();
        //创建一个服务端连接
        bool CreateServer(uint16_t port, const std::string &ip = "0.0.0.0", bool block_flag = false);
        //创建一个客户端连接
        bool CreateClient(uint16_t port, const std::string &ip);
        //设置套接字选项---开启地址端口重用
        void ReuseAddress();
        //设置套接字阻塞属性-- 设置为非阻塞
        bool NonBlock();
};
