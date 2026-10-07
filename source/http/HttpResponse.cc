#include "HttpResponse.hpp"
#include <utility>

HttpResponse::HttpResponse():_redirect_flag(false), _statu(200) {}

HttpResponse::HttpResponse(int statu):_redirect_flag(false), _statu(statu) {}

void HttpResponse::ReSet() {
    _statu = 200;
    _redirect_flag = false;
    _body.clear();
    _redirect_url.clear();
    _headers.clear();
}

void HttpResponse::SetHeader(const std::string &key, const std::string &val) {
    _headers[key] = val;
}

bool HttpResponse::HasHeader(const std::string &key) {
    auto it = _headers.find(key);
    if (it == _headers.end()) {
        return false;
    }
    return true;
}

std::string HttpResponse::GetHeader(const std::string &key) {
    auto it = _headers.find(key);
    if (it == _headers.end()) {
        return "";
    }
    return it->second;
}

void HttpResponse::SetContent(const std::string &body,  const std::string &type) {
    _body = body;
    SetHeader("Content-Type", type);
}

void HttpResponse::SetRedirect(const std::string &url, int statu) {
    _statu = statu;
    _redirect_flag = true;
    _redirect_url = url;
}

bool HttpResponse::Close() {
    // 没有Connection字段，或者有Connection但是值是close，则都是短链接，否则就是长连接
    if (HasHeader("Connection") == true && GetHeader("Connection") == "keep-alive") {
        return false;
    }
    return true;
}
