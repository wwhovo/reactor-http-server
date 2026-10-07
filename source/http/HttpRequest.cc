#include "HttpRequest.hpp"
#include <utility>
#include <algorithm>
#include <cctype>
#include <limits>

namespace {
std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
}

HttpRequest::HttpRequest():_version("HTTP/1.1") {}

void HttpRequest::ReSet() {
    _method.clear();
    _path.clear();
    _version = "HTTP/1.1";
    std::string().swap(_body); // 小文件上传结束后归还正文内存。
    std::smatch match;
    _matches.swap(match);
    _headers.clear();
    _params.clear();
}

void HttpRequest::SetHeader(const std::string &key, const std::string &val) {
    _headers[Lower(key)] = val;
}

bool HttpRequest::HasHeader(const std::string &key) const {
    auto it = _headers.find(Lower(key));
    if (it == _headers.end()) {
        return false;
    }
    return true;
}

std::string HttpRequest::GetHeader(const std::string &key) const {
    auto it = _headers.find(Lower(key));
    if (it == _headers.end()) {
        return "";
    }
    return it->second;
}

void HttpRequest::SetParam(const std::string &key, const std::string &val) {
    _params.insert(std::make_pair(key, val));
}

bool HttpRequest::HasParam(const std::string &key) const {
    auto it = _params.find(key);
    if (it == _params.end()) {
        return false;
    }
    return true;
}

std::string HttpRequest::GetParam(const std::string &key) const {
    auto it = _params.find(key);
    if (it == _params.end()) {
        return "";
    }
    return it->second;
}

size_t HttpRequest::ContentLength() const {
    size_t length = 0;
    return TryContentLength(&length) ? length : 0;
}

bool HttpRequest::TryContentLength(size_t *length) const {
    *length = 0;
    if (!HasHeader("Content-Length")) return true;
    const std::string value = GetHeader("Content-Length");
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (c < '0' || c > '9' || *length > (std::numeric_limits<size_t>::max() - (c - '0')) / 10)//计算最后一位时，不能超过size_t最大值的最后一位
        *length = *length * 10 + c - '0';
    }
    return true;
}

bool HttpRequest::Close() const {
    const std::string connection = Lower(GetHeader("Connection"));
    if (connection.find("close") != std::string::npos) return true;
    if (connection.find("keep-alive") != std::string::npos) return false;

    return _version == "HTTP/1.0"; //1.0直接关闭,1.1返回false不关闭
}
