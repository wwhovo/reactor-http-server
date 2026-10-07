#pragma once
#include <cstddef>
#include <regex>
#include <string>
#include <unordered_map>

class HttpRequest {
    public:
        std::string _method;      //请求方法
        std::string _path;        //资源路径
        std::string _version;     //协议版本
        std::string _body;        //请求正文
        std::smatch _matches;     //资源路径的正则提取数据
        std::unordered_map<std::string, std::string> _headers;  //头部字段
        std::unordered_map<std::string, std::string> _params;   //查询字符串
    public:
        HttpRequest();
        void ReSet();
        //插入头部字段
        void SetHeader(const std::string &key, const std::string &val);
        //判断是否存在指定头部字段
        bool HasHeader(const std::string &key) const;
        //获取指定头部字段的值
        std::string GetHeader(const std::string &key) const;
        //插入查询字符串
        void SetParam(const std::string &key, const std::string &val);
        //判断是否有某个指定的查询字符串
        bool HasParam(const std::string &key) const;
        //获取指定的查询字符串
        std::string GetParam(const std::string &key) const;
        //获取正文长度
        size_t ContentLength() const;
        bool TryContentLength(size_t *length) const;
        //判断是否是短链接
        bool Close() const;
};
