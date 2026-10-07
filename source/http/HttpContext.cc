#include "HttpContext.hpp"
#include "Util.hpp"
#include <algorithm>
#include <cctype>

// HttpContext 是“每条连接上的 HTTP 增量解析器”，不直接调用 socket 的 recv。
// Buffer 保留尚未消费的原始字节；HttpContext 保留已解析字段和当前处理阶段。
// 正常状态：LINE（请求行）→ HEAD（请求头）→ BODY（正文）→ OVER（输入完整）。
// 解析失败：Fail 设置错误状态码，并进入 ERROR；回复和关闭连接由 HttpServer 负责。
// 注意：Recv 系列函数返回 true，可能只是“暂时没有错误，等后续数据”，
// 是否完整必须看 RecvStatu()，不能仅根据函数返回值判断。

namespace {
// 校验尚未 URL 解码的请求目标，例如 /hello?name=C%2B%2B。
// 本函数只检查原始字符和 %HH 格式；解码后的控制字符由 Clean 再检查。
// 匿名命名空间让这两个辅助函数只供当前编译单元内部使用。
bool ValidEncoded(const std::string &value) {
    for (size_t i = 0; i < value.size(); ++i) {
        // 使用无符号字符，避免高位字节以负值传入字符分类函数。
        unsigned char c = value[i];
        // 原始目标不能含未编码的空格、控制字符（含 NUL）或 DEL。
        if (c <= 32 || c == 127) return false;
        if (c == '%') {
            // 百分号后必须还有两个字符，且两者都是十六进制数字。
            // 先判断长度，再访问 i+1/i+2，利用 || 的短路求值避免越界。
            if (i + 2 >= value.size() || !std::isxdigit(static_cast<unsigned char>(value[i + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(value[i + 2]))) return false;
            // 两位数字已经检查过；跳过它们，下一轮继续检查后面的字节。
            i += 2;
        }
    }
    return true;
}

// 检查 URL 解码后的路径或参数，拒绝 NUL、换行等控制字符及 DEL。
// 例如 %00/%0A 原始格式合法，但解码结果不允许出现在路径/参数中。
// 与 ValidEncoded 不同，这里允许空格（例如由 %20 解码得到的空格）。
// 这不是文件路径沙箱检查，也不检查 .. 等目录穿越语义。
bool Clean(const std::string &value) {
    for (unsigned char c : value) if (c < 32 || c == 127) return false;
    return true;
}
}

// 统一记录解析错误：保存稍后回复的 HTTP 状态码，并终止正常状态推进。
// 返回 false，方便调用者直接写 return Fail(400)；这里不发送响应、不关 socket。
bool HttpContext::Fail(int status) {
    _resp_statu = status;
    _recv_statu = RECV_HTTP_ERROR;
    return false;
}

// 解析一条已经从 Buffer 取出的完整请求行（包含结尾 CRLF）。
// 例如：GET /hello?name=alice HTTP/1.1\r\n。
// 负责填充 Request 的方法、版本、路径、参数；状态迁移由 RecvHttpLine 负责。
bool HttpContext::ParseHttpLine(const std::string &line) {
    // Buffer 按 LF 取行，但 HTTP 请求行在这里要求以 CRLF（两个字节）结束。
    if (line.size() < 2 || line.substr(line.size() - 2) != "\r\n") return Fail(400);
    // 去掉结尾 CRLF，后续只分析请求行本身，不把换行混入版本字段。
    const std::string raw = line.substr(0, line.size() - 2);
    // 第一个空格前是方法，最后一个空格后是版本，中间是请求目标。
    const size_t first = raw.find(' '), last = raw.rfind(' ');
    // 无空格或只有一个空格，无法分出三个部分，属于非法请求行。
    if (first == std::string::npos || first == last) return Fail(400);
    _request._method = raw.substr(0, first);
    _request._version = raw.substr(last + 1);
    const std::string target = raw.substr(first + 1, last - first - 1);

    // 当前实现只支持这两个版本和下面五种方法，不是完整 HTTP 协议实现。
    if (_request._version != "HTTP/1.0" && _request._version != "HTTP/1.1") return Fail(400);
    if (_request._method != "GET" && _request._method != "HEAD" && _request._method != "POST" &&
        _request._method != "PUT" && _request._method != "DELETE") return Fail(405);
    // 只接收以 / 开头的请求目标；拒绝片段标记 # 及不合法的原始编码。
    if (target.empty() || target[0] != '/' || target.find('#') != std::string::npos || !ValidEncoded(target))
        return Fail(400);

    // 先定位 ? 分开路径和查询串，再分别解码，避免编码字符被误认成分隔符。
    const size_t pos = target.find('?');
    // pos 为 npos 时，substr(0, pos) 会取得整个目标，即没有查询串的路径。
    // false 表示路径中的 + 保持为 +，不把它当作空格。
    _request._path = Util::UrlDecode(target.substr(0, pos), false);
    if (!Clean(_request._path)) return Fail(400);
    if (pos != std::string::npos) {
        std::vector<std::string> params;
        // 例如 name=alice&age=20 → 两个键值参数；Split 的当前策略会忽略空段。
        Util::Split(target.substr(pos + 1), "&", &params);
        for (const auto &param : params) {
            // 只按第一个 = 拆分，后面的 = 可作为参数值的一部分。
            const size_t equal = param.find('=');
            // 当前实现要求每个非空参数段都带 =；name= 可以表示空值。
            if (equal == std::string::npos) return Fail(400);
            // true 表示查询参数中的 + 按空格处理；%HH 同时还原成字节。
            // 先拆结构再解码，保证值中的 %26 不会被误当成另一个参数的起点。
            const std::string key = Util::UrlDecode(param.substr(0, equal), true);
            const std::string value = Util::UrlDecode(param.substr(equal + 1), true);
            if (!Clean(key) || !Clean(value)) return Fail(400);
            // 写入 Request::_params；当前 SetParam 对重复键保留先插入的值。
            _request.SetParam(key, value);
        }
    }
    return true;
}

// 请求行阶段入口：从 Buffer 取一整行，解析成功后转入 HEAD。
// 半行数据继续保留在 Buffer，等待下一次 OnMessage 调用，不立即判错。
bool HttpContext::RecvHttpLine(Buffer *buf) {
    // 阶段保护：请求行已经处理过时，不再重复消费字节。
    if (_recv_statu != RECV_HTTP_LINE) return false;
    // 有 LF 才会取出并推进读偏移；没有完整行则返回空字符串、不消费半行。
    const std::string line = buf->GetLineAndPop();//读到\r\n则为完整一行
    // 没有完整行：未超限则暂时等待；半行已超过 MAX_LINE 则报 414。
    if (line.empty()) return buf->ReadAbleSize() > MAX_LINE ? Fail(414) : true;
    // 完整行同样检查上限，避免一次收到超长请求行时绕过半行检查。
    if (line.size() > MAX_LINE) return Fail(414);
    if (!ParseHttpLine(line)) return false;
    // 只有请求行合法才推进状态，后续可继续从同一个 Buffer 解析头部。
    _recv_statu = RECV_HTTP_HEAD;
    return true;
}

// 解析一行非空请求头，例如 Content-Length: 3\r\n，保存到 Request::_headers。
// line 是非 const 引用：本函数会原地去掉其 CRLF；头部结束空行由 RecvHttpHead 处理。
bool HttpContext::ParseHttpHead(std::string &line) {
    if (line.size() < 2 || line.substr(line.size() - 2) != "\r\n") return Fail(400);
    line.resize(line.size() - 2);
    // 按第一个冒号分出字段名和值；值中的其他冒号仍保留，例如 Host: IP:端口。
    const size_t colon = line.find(':');
    // 必须有冒号，而且字段名不能为空。
    if (colon == std::string::npos || colon == 0) return Fail(400);
    const std::string key = line.substr(0, colon);
    // 检查字段名字符：字母数字或这里列出的标点；不允许空格等混入字段名。
    for (unsigned char c : key) {
        if (!std::isalnum(c) && std::string("!#$%&'*+-.^_`|~").find(c) == std::string::npos) return Fail(400);
    }
    std::string value = line.substr(colon + 1);
    // 只去掉值两端的空格/水平制表符，不改变中间内容或 Token 的大小写。
    const size_t first = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t");
    // 全是空白时 first 为 npos，保存空值，避免用无效位置截取字符串。
    value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
    // 字段值允许水平制表符，但拒绝其他控制字符与 DEL。
    for (unsigned char c : value) if ((c < 32 && c != '\t') || c == 127) return Fail(400);
    // HasHeader/SetHeader 会统一字段名大小写，所以 Host 与 host 也算重复。
    // 一律拒绝重复字段是本项目的保守策略，不代表所有 HTTP 字段都禁止重复。
    if (_request.HasHeader(key)) return Fail(400); // 拒绝重复字段，避免消息长度歧义。
    _request.SetHeader(key, value);
    return true;
}

// 请求头阶段入口：尽量消费 Buffer 中的完整头行，遇到 CRLF 空行结束头部。
// 所有头完整之后，再统一检查消息边界、Host、Content-Length 和声明大小。
bool HttpContext::RecvHttpHead(Buffer *buf) {
    if (_recv_statu != RECV_HTTP_HEAD) return false;
    while (true) {
        std::string line = buf->GetLineAndPop();
        // line.empty() 是“尚未收到一整行”，不是头部结束标记 "\r\n"。
        if (line.empty()) {
            // 已消费的完整头行计数 + 尚未完整的这一行，也不能超过总上限。
            if (buf->ReadAbleSize() > MAX_LINE 
            || _header_bytes + buf->ReadAbleSize() > 32768) return Fail(431);
            return true;
        }
        // 累计头部字节（包含各行 CRLF 和最终空行），限制单行 8 KiB、总计 32 KiB。
        _header_bytes += line.size();
        if (line.size() > MAX_LINE || _header_bytes > 32768) return Fail(431);
        // 只有 CRLF 的一行表示头部结束；不把它当成字段解析或计入字段个数。
        if (line == "\r\n") break;
        // 每条非空头行算一个字段，当前最多接受 100 个。
        if (++_header_count > 100) return Fail(431);
        if (!ParseHttpHead(line)) return false;
    }

    // 不支持 Transfer-Encoding（包括 chunked）：单独出现报 501。
    // 同时带 Content-Length 时拒绝为 400，避免对正文边界作两种解释。
    if (_request.HasHeader("Transfer-Encoding"))
        return Fail(_request.HasHeader("Content-Length") ? 400 : 501);
    // HTTP/1.1 请求在本实现中必须带非空 Host；HTTP/1.0 不做此强制检查。
    if (_request._version == "HTTP/1.1" && _request.GetHeader("Host").empty()) return Fail(400);
    size_t length;
    // 必须用 TryContentLength 判断是否合法：非数字/溢出失败；缺字段按长度 0。
    // 上传 API 是否必须提供长度，后面由业务预检额外判断。
    if (!_request.TryContentLength(&length)) return Fail(400);
    // 只根据声明长度提前拒绝超限请求，不必先把整个正文收完。
    if (length > _max_body_size) return Fail(413);
    // 头部完整且检查通过，即使正文为 0 也先进入 BODY，由正文入口推进到 OVER。
    _recv_statu = RECV_HTTP_BODY;
    return true;
}

// 正文阶段入口：只消费本请求 Content-Length 范围内的字节。
// 第一遍学习先看 else 的普通正文聚合；_consumer 分支是 M2 的流式扩展。
bool HttpContext::RecvHttpBody(Buffer *buf) {
    if (_recv_statu != RECV_HTTP_BODY) return false;
    // 长度已在头阶段校验；_body_received 只累加已接受且不超过剩余长度的字节。
    const size_t remaining = _request.ContentLength() - _body_received;
    // 不能直接拿走整个 Buffer：其中可能还有下一个请求，或当前正文只有一部分。
    size_t bytes = std::min(remaining, static_cast<size_t>(buf->ReadAbleSize()));
    if (_consumer) {
        // 【流式扩展】一次最多交给业务 64 KiB，不把整个文件拼进 Request::_body。
        bytes = std::min<size_t>(bytes, 65536);
        // 正文未收完且没有新字节时等待；remaining=0 时不能直接返回，
        // 空文件仍需要 consumer(data, 0, true) 执行最终提交。
        if (!bytes && remaining) return true;
        // bytes==remaining 表示最后一块。consumer 返回 true 才算接受本块；
        // 返回 false（例如等待磁盘准备或无法接受）时，不推进计数和 Buffer 偏移。
        // data 指向 Buffer，异步业务若稍后使用，必须自行复制/持有这些字节。
        if (!_consumer(buf->ReadPosition(), bytes, bytes == remaining)) return true;
    } else {
        // 大文件只允许走流式消费者；普通路由仍保持小正文接口。
        // 即使 _max_body_size 配得更大，未安装 consumer 的聚合正文仍限制为 8 MiB。
        if (_request.ContentLength() > 8 * 1024 * 1024) return Fail(413);
        // 按明确字节数追加，正文中有 NUL 或 CRLF 也不会被当作结束标记。
        if (bytes) _request._body.append(buf->ReadPosition(), bytes);
    }
    // 两种路径都用独立计数统计输入进度，不能再依赖 _body.size() 判断流式进度。
    _body_received += bytes;
    // 只有被接受的字节才从输入 Buffer 消费掉，其余留给后续调用/下一请求。
    buf->MoveReadOffset(bytes);
    // OVER 只表示本请求输入完整，不等于磁盘写入/发布完成或响应已发送到对端。
    if (_body_received == _request.ContentLength()) _recv_statu = RECV_HTTP_OVER;
    return true;
}

// 新建一条连接的解析上下文：初始无错误，从请求行开始，头部计数归零。
// max_body_size 控制声明正文上限；HttpServer 可以按业务配置它。
// _body_received/_busy 在头文件中有初值；智能指针/函数对象默认不持有资源或回调。
HttpContext::HttpContext(size_t max_body_size): _resp_statu(200), _recv_statu(RECV_HTTP_LINE),
    _header_bytes(0), _header_count(0), _max_body_size(max_body_size), _headers_checked(false) {}

// 清理上一请求的状态，准备在同一连接上解析下一请求；连接关闭时也会调用。
// 本函数不清空 Connection 的输入 Buffer，不关闭 socket，也不重置正文配置上限。
void HttpContext::ReSet() {
    // 【流式扩展】把取消回调移到局部保活，清空正文消费者后再通知传输对象收尾。
    // 正常完成也走这里；Transfer 会依据完成状态决定是否还需要取消/清理。
    auto cancel = std::move(_cancel);
    _consumer = nullptr;
    if (cancel) cancel();

    // 清除正文输入进度、异步响应等待标记、错误码和解析阶段。
    _body_received = 0;
    _busy = false;
    _resp_statu = 200;
    _recv_statu = RECV_HTTP_LINE;
    // 每份请求独立计算头部限制，并在头部完整后重新执行业务预检。
    _header_bytes = _header_count = 0;
    _headers_checked = false;
    // 放弃 Context 持有的资源票据；如果磁盘清理任务还持有同一票据，
    // 名额不会在这里提前归还，而是等最后一个 shared_ptr 引用释放。
    _guard.reset();
    // 清空旧方法、路径、头、参数、正文和匹配结果，避免污染下一请求。
    _request.ReSet();
}

// 取得当前解析结果状态码：初始 200，Fail 后用于生成相应错误响应。
int HttpContext::RespStatu() { return _resp_statu; }

// 取得解析阶段：调用者据此区分“继续等待”“请求完整”与“解析失败”。
HttpRecvStatu HttpContext::RecvStatu() { return _recv_statu; }

// 返回 Context 内部 Request 的引用，不复制整份请求；业务可读取解析结果。
// ReSet 后这份对象的字段会被清空，异步任务若需要旧数据，应先复制所需内容。
HttpRequest &HttpContext::Request() { return _request; }

// 对外增量解析入口，由 HttpServer::OnMessage 驱动；一次调用尝试推进当前请求。
// parse_body=false：只解析到头部结束，留机会给鉴权/准入/安装流式消费者。
// parse_body=true：允许继续消费正文。这里不负责路由、发送或循环解析下一请求。
void HttpContext::RecvHttpRequest(Buffer *buf, bool parse_body) {
    // 使用连续 if 而不是 else if：前一个函数更新状态后，同一次调用可以
    // 从 LINE 继续进入 HEAD，再进入 BODY，利用 Buffer 中已经到达的后续字节。
    // 半行未完整时状态不变；Fail 转入 ERROR 后也不会再满足后续阶段条件。
    if (_recv_statu == RECV_HTTP_LINE) RecvHttpLine(buf);
    if (_recv_statu == RECV_HTTP_HEAD) RecvHttpHead(buf);
    if (parse_body && _recv_statu == RECV_HTTP_BODY) RecvHttpBody(buf);
}
