#include "Socket.hpp"

Socket::Socket():_sockfd(-1) { EnsureNetworkInitialized(); }

Socket::Socket(int fd): _sockfd(fd) { EnsureNetworkInitialized(); }

Socket::~Socket() { Close(); }

int Socket::Fd() { return _sockfd; }

uint16_t Socket::LocalPort() {
    sockaddr_in addr = {};
    socklen_t len = sizeof(addr);
    if (getsockname(_sockfd, reinterpret_cast<sockaddr *>(&addr), &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

bool Socket::Create() {
    // int socket(int domain, int type, int protocol)
    Close();
    _sockfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (_sockfd < 0) {
        ERR_LOG("CREATE SOCKET FAILED!!");
        return false;
    }
    return true;
}

bool Socket::Bind(const std::string &ip, uint16_t port) {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        errno = EINVAL;
        return false;
    }
    socklen_t len = sizeof(struct sockaddr_in);
    // int bind(int sockfd, struct sockaddr*addr, socklen_t len);
    int ret = bind(_sockfd, (struct sockaddr*)&addr, len);
    if (ret < 0) {
        ERR_LOG("BIND ADDRESS FAILED!");
        return false;
    }
    return true;
}

bool Socket::Listen(int backlog) {
    // int listen(int backlog)
    int ret = listen(_sockfd, backlog);
    if (ret < 0) {
        ERR_LOG("SOCKET LISTEN FAILED!");
        return false;
    }
    return true;
}

bool Socket::Connect(const std::string &ip, uint16_t port) {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    socklen_t len = sizeof(struct sockaddr_in);
    // int connect(int sockfd, struct sockaddr*addr, socklen_t len);
    int ret = connect(_sockfd, (struct sockaddr*)&addr, len);
    if (ret < 0) {
        ERR_LOG("CONNECT SERVER FAILED!");
        return false;
    }
    return true;
}

int Socket::Accept() {
    // int accept(int sockfd, struct sockaddr *addr, socklen_t *len);
    int newfd;
    do { newfd = accept4(_sockfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC); }
    while (newfd < 0 && errno == EINTR);
    if (newfd < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ERR_LOG("SOCKET ACCEPT FAILED!");
        return -1;
    }
    return newfd;
}

ssize_t Socket::Recv(void *buf, size_t len, int flag) {
    // ssize_t recv(int sockfd, void *buf, size_t len, int flag);
    ssize_t ret;
    do { ret = recv(_sockfd, buf, len, flag); } while (ret < 0 && errno == EINTR);
    // 保留系统调用语义：0 为 EOF，-1 + EAGAIN 为暂时不可读。
    return ret;
}

ssize_t Socket::NonBlockRecv(void *buf, size_t len) {
    return Recv(buf, len, MSG_DONTWAIT); // MSG_DONTWAIT 表示当前接收为非阻塞。
}

ssize_t Socket::Send(const void *buf, size_t len, int flag) {
    // ssize_t send(int sockfd, void *data, size_t len, int flag);
    ssize_t ret;
    do { ret = send(_sockfd, buf, len, flag | MSG_NOSIGNAL); }
    while (ret < 0 && errno == EINTR);
    return ret;
}

ssize_t Socket::NonBlockSend(void *buf, size_t len) {
    if (len == 0) return 0;
    return Send(buf, len, MSG_DONTWAIT); // MSG_DONTWAIT 表示当前发送为非阻塞。
}

void Socket::Close() {
    if (_sockfd != -1) {
        close(_sockfd);
        _sockfd = -1;
    }
}

bool Socket::CreateServer(uint16_t port, const std::string &ip, bool block_flag) {
    //地址/端口复用选项必须在 bind 之前设置，服务才能及时重启。
    if (Create() == false) return false;
    if (block_flag && !NonBlock()) { Close(); return false; }
    ReuseAddress();
    if (!Bind(ip, port) || !Listen()) { Close(); return false; }
    return true;
}

bool Socket::CreateClient(uint16_t port, const std::string &ip) {
    //1. 创建套接字，2.指向连接服务器
    if (Create() == false) return false;
    if (Connect(ip, port) == false) { Close(); return false; }
    return true;
}

void Socket::ReuseAddress() {
    // int setsockopt(int fd, int leve, int optname, void *val, int vallen)
    int val = 1;
    setsockopt(_sockfd, SOL_SOCKET, SO_REUSEADDR, (void*)&val, sizeof(int));
    // 单实例服务只需要地址复用；不默认允许多个进程分享同一监听端口。
}

bool Socket::NonBlock() {
    //int fcntl(int fd, int cmd, ... /* arg */ );
    int flag = fcntl(_sockfd, F_GETFL, 0);
    return flag >= 0 && fcntl(_sockfd, F_SETFL, flag | O_NONBLOCK) == 0;
}
