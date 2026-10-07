#include "../source/http/HttpContext.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

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
        HttpRequest request;
        size_t parsed = 123;
        Check(request.TryContentLength(&parsed) && parsed == 0, "missing length is zero");
        const size_t maximum = std::numeric_limits<size_t>::max();
        const std::vector<std::pair<std::string, size_t>> valid_lengths = {
            {"0", 0}, {"3", 3}, {"0003", 3}, {std::to_string(maximum), maximum}
        };
        for (const auto &entry : valid_lengths) {
            request.SetHeader("Content-Length", entry.first);
            Check(request.TryContentLength(&parsed) && parsed == entry.second, "decimal length");
        }
        for (const auto &length : {std::string(""), std::string("-1"), std::string("+1"),
                                  std::string("1x"), std::to_string(maximum) + "0"}) {
            request.SetHeader("Content-Length", length);
            Check(!request.TryContentLength(&parsed), "invalid or overflowing decimal length");
        }
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
        context.ReSet();
        input.WriteStringAndPush(line + "Content-Length: 3\r\n\r\nabc"
                                 "GET /after-body HTTP/1.1\r\nHost: localhost\r\n\r\n");
        context.RecvHttpRequest(&input);
        Check(context.RecvStatu() == RECV_HTTP_OVER && context.Request()._body == "abc" &&
              input.ReadAbleSize() > 0, "POST body must not consume the next request");
        context.ReSet(); context.RecvHttpRequest(&input);
        Check(context.Request()._path == "/after-body" && input.ReadAbleSize() == 0,
              "GET after pipelined POST");
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
