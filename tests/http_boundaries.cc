#include "../source/http/HttpContext.hpp"
#include <iostream>
#include <stdexcept>

static void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
static void Bad(const std::string &wire, int status, size_t max_body = 8388608) {
    HttpContext context(max_body);
    Buffer input;
    input.WriteStringAndPush(wire);
    context.RecvHttpRequest(&input);
    Check(context.RecvStatu() == RECV_HTTP_ERROR && context.RespStatu() == status, wire.substr(0, 100).c_str());
}
int main() {
    try {
        const std::string line = "POST /login HTTP/1.1\r\nHost: localhost\r\n";
        for (const auto &length : {"-1", "+1", "x", "1x", "", "184467440737095516160"})
            Bad(line + "Content-Length: " + length + "\r\n\r\n", 400);
        Bad(line + "Content-Length: 1\r\ncontent-length: 1\r\n\r\n", 400);
        Bad(line + "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n", 400);
        Bad(line + "Transfer-Encoding: chunked\r\n\r\n", 501);
        Bad(line + "Content-Length: 9\r\n\r\n", 413, 8);
        Bad("GET /%xy HTTP/1.1\r\n\r\n", 400);
        Bad("GET /%00 HTTP/1.1\r\n\r\n", 400);
        Bad("GET / HTTP/1.1\n\n", 400);
        Bad(line + ":\r\n\r\n", 400);
        Bad("GET /" + std::string(8200, 'x') + " HTTP/1.1\r\n\r\n", 414);
        Bad(line + "X:" + std::string(8200, 'x'), 431);
        std::string headers;
        for (int i = 0; i < 101; ++i) headers += "X-" + std::to_string(i) + ":a\r\n";
        Bad(line + headers + "\r\n", 431);
        headers.clear();
        for (int i = 0; i < 5; ++i) headers += "X-" + std::to_string(i) + ":" + std::string(7000, 'a') + "\r\n";
        Bad(line + headers + "\r\n", 431);
        HttpContext context(8);
        Buffer input;
        const std::string wire = "POST /hello?name=alice HTTP/1.1\r\nHost: localhost\r\ncontent-length:3\r\n\r\nabc";
        for (char byte : wire) { input.WriteAndPush(&byte, 1); context.RecvHttpRequest(&input); }
        Check(context.RecvStatu() == RECV_HTTP_OVER && context.Request()._body == "abc", "fragmented request");
        Check(context.Request().GetHeader("Content-Length") == "3", "case-insensitive headers");
        context.ReSet();
        input.WriteStringAndPush("GET / HTTP/1.1\r\nHost: localhost\r\n\r\nGET /next HTTP/1.1\r\nHost: localhost\r\n\r\n");
        context.RecvHttpRequest(&input);
        Check(context.RecvStatu() == RECV_HTTP_OVER && input.ReadAbleSize() > 0, "first pipelined request");
        context.ReSet(); context.RecvHttpRequest(&input);
        Check(context.Request()._path == "/next" && input.ReadAbleSize() == 0, "second pipelined request");
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
