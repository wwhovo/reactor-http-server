#pragma once
#include "HttpRequest.hpp"
#include "../net/Buffer.hpp"
#include <memory>
#include <functional>

typedef enum {
    RECV_HTTP_ERROR,
    RECV_HTTP_LINE,
    RECV_HTTP_HEAD,
    RECV_HTTP_BODY,
    RECV_HTTP_OVER
}HttpRecvStatu;

#define MAX_LINE 8192

class HttpContext {
    private:
        int _resp_statu; //响应状态码
        HttpRecvStatu _recv_statu; //当前接收及解析的阶段状态
        HttpRequest _request;  //已经解析得到的请求信息
        size_t _header_bytes;
        size_t _header_count;
        size_t _max_body_size;
        bool _headers_checked;
        std::shared_ptr<void> _guard; // 请求级资源随请求/连接释放。
        size_t _body_received = 0;
        bool _busy = false;
        std::function<bool(const char *, size_t, bool)> _consumer;
        std::function<void()> _cancel;
    private:
        bool ParseHttpLine(const std::string &line);
        bool RecvHttpLine(Buffer *buf);
        bool RecvHttpHead(Buffer *buf);
        bool ParseHttpHead(std::string &line);
        bool RecvHttpBody(Buffer *buf);
        bool Fail(int status);
    public:
        explicit HttpContext(size_t max_body_size = 8 * 1024 * 1024);
        void ReSet();
        int RespStatu();
        HttpRecvStatu RecvStatu();
        HttpRequest &Request();
        //接收并解析HTTP请求
        void RecvHttpRequest(Buffer *buf, bool parse_body = true);
        bool HeadersChecked() const { return _headers_checked; }
        void MarkHeadersChecked() { _headers_checked = true; }
        void SetGuard(const std::shared_ptr<void> &guard) { _guard = guard; }
        std::shared_ptr<void> Guard() const { return _guard; }
        void SetConsumer(const std::function<bool(const char *, size_t, bool)> &f) { _consumer = f; }
        bool Streaming() const { return static_cast<bool>(_consumer); }
        void SetCancel(const std::function<void()> &f) { _cancel = f; }
        bool Busy() const { return _busy; }
        void SetBusy() { _busy = true; }
};
