#include "http.hpp"

#define WWWROOT "./wwwroot/"

std::string RequestStr(const HttpRequest &req) {
    std::stringstream ss;
    ss << req._method << " " << req._path << " " << req._version << "\r\n";
    for (auto &it : req._params) {
        ss << it.first << ": " << it.second << "\r\n";
    }
    for (auto &it : req._headers) {
        ss << it.first << ": " << it.second << "\r\n";
    }
    ss << "\r\n";
    ss << req._body;
    return ss.str();
}
void Hello(const HttpRequest &req, HttpResponse *rsp) 
{
    rsp->SetContent(RequestStr(req), "text/plain");
}
void Login(const HttpRequest &req, HttpResponse *rsp) 
{
    rsp->SetContent(RequestStr(req), "text/plain");
}
int main()
{
    // 默认只提供本机演示，避免无认证服务被所有网卡访问。
    HttpServer server(8085, DEFALT_TIMEOUT, "127.0.0.1");
    server.SetThreadCount(3);
    server.EnableSignalStop(); // 在工作线程启动前统一接收 SIGINT/SIGTERM，正常回收资源。
    server.SetBaseDir(WWWROOT);//设置静态资源根目录，告诉服务器有静态资源请求到来，需要到哪里去找资源文件
    server.Get("/hello", Hello);
    server.Post("/login", Login);
    server.Listen();
    return 0;
}
