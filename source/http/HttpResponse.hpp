#pragma once
#include <string>
#include <unordered_map>

class HttpResponse {
    public:
        int _statu;
        bool _redirect_flag;
        std::string _body;
        std::string _redirect_url; //客户端下一次去的地址。浏览器通常会自动跟随
        std::unordered_map<std::string, std::string> _headers;
    public:
        HttpResponse();
        HttpResponse(int statu); 
        void ReSet();
        //插入头部字段
        void SetHeader(const std::string &key, const std::string &val);
        //判断是否存在指定头部字段
        bool HasHeader(const std::string &key);
        //获取指定头部字段的值
        std::string GetHeader(const std::string &key);
        void SetContent(const std::string &body,  const std::string &type = "text/html");
        void SetRedirect(const std::string &url, int statu = 302);
        //判断是否是短链接
        bool Close();
};
