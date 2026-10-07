#include "../source/http/http.hpp"

bool OtherTranslationUnitWorks();

int main() {
    if (!OtherTranslationUnitWorks()) return 1;
    if (Util::ExtMime("index.html") != "text/html") return 2;

    HttpContext context;
    Buffer buffer;
    buffer.WriteStringAndPush(
        "POST /login?name=alice HTTP/1.1\r\n"
        "Host: localhost\r\nContent-Length: 3\r\n\r\nx");
    context.RecvHttpRequest(&buffer);
    if (context.RecvStatu() != RECV_HTTP_BODY) return 3;
    buffer.WriteStringAndPush("=1");
    context.RecvHttpRequest(&buffer);
    if (context.RecvStatu() != RECV_HTTP_OVER) return 4;
    if (context.Request()._method != "POST") return 5;
    if (context.Request().GetParam("name") != "alice") return 6;
    if (context.Request()._body != "x=1") return 7;
}
