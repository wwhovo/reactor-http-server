#include "HttpServer.hpp"
#include "Util.hpp"
#include "../base/Logging.hpp"
#include <cassert>
#include <sstream>

// 阅读主线：构造函数注册回调 → OnConnected 建上下文 → OnMessage 驱动解析。
// 普通业务接着走 Route/Dispatcher → 填 HttpResponse → WriteReponse → Connection::Send。
// 【扩展】异步业务不走普通路由表，稍后回所属 loop 调用 Reply 或 Begin/EndResponse。
// 本类的连接处理回调运行在连接所属 EventLoop；它不是磁盘工作线程池。

// 将当前错误状态码组织成 HTML 页面，保存到响应对象中，尚未向网络发送。
// req 在当前页面实现中未使用；响应状态码应由调用方事先设置。
void HttpServer::ErrorHandler(const HttpRequest &req, HttpResponse *rsp) {
    // 1. 组织一个错误展示页面，例如页面标题中的 400 Bad Request。
    std::string body;
    body += "<html>";
    body += "<head>";
    body += "<meta http-equiv='Content-Type' content='text/html;charset=utf-8'>";
    body += "</head>";
    body += "<body>";
    body += "<h1>";
    body += std::to_string(rsp->_statu);
    body += " ";
    body += Util::StatuDesc(rsp->_statu);
    body += "</h1>";
    body += "</body>";
    body += "</html>";
    // 2. SetContent 保存正文并设置 Content-Type；发送由外层 WriteReponse 完成。
    rsp->SetContent(body, "text/html");
}

// 响应序列化入口：补必要字段 → 状态行 → 头部 → 空行 → 正文 → 交给 Connection。
// 既用于普通完整响应，也用于流式响应的头部；名字沿用原来的 WriteReponse 拼写。
void HttpServer::WriteReponse(const PtrConnection &conn, const HttpRequest &req, HttpResponse &rsp) {
    // 1. 业务显式设置 Connection 时优先使用它，否则采用请求的关闭/保持连接策略。
    // rsp.Close() 依赖此字段，因此要在补齐响应头之后再决定是否 Shutdown。
    if (!rsp.HasHeader("Connection")) rsp.SetHeader("Connection", req.Close() ? "close" : "keep-alive");
    if (!rsp.HasHeader("Content-Length")) {
        // 普通响应按正文字节数计算；不能用 strlen，因为正文可能包含 NUL。
        // 流式下载事先设置文件/片段长度，虽 _body 为空，这里也不会覆盖它。
        // 现有边界：空正文统一补长度，尚未专门处理 204 不应携带该字段的情况。
        rsp.SetHeader("Content-Length", std::to_string(rsp._body.size()));
    }
    // 有正文却没指定类型时，兜底作为二进制字节流，不改变正文内容。
    if (rsp._body.empty() == false && rsp.HasHeader("Content-Type") == false) {
        rsp.SetHeader("Content-Type", "application/octet-stream");
    }
    // 重定向地址写入 Location；是否跟随并重新请求，由客户端决定。
    if (rsp._redirect_flag == true) {
        rsp.SetHeader("Location", rsp._redirect_url);
    }
    // 响应字段容器目前区分键名大小写，业务应使用上述统一拼写，避免重复补头。

    // 2. 状态行形如 HTTP/1.1 200 OK\r\n；解析错误时版本不合法则兜底 1.1。
    std::stringstream rsp_str;//stringstream它把字符串当成流来读写，支持 << 和 >> 运算符，像 cin/cout 一样方便。
    const std::string version = req._version == "HTTP/1.0" ? "HTTP/1.0" : "HTTP/1.1";
    rsp_str << version << " " << std::to_string(rsp._statu) << " " << Util::StatuDesc(rsp._statu) << "\r\n";
    // 每个头字段序列化为“名称: 值 + CRLF”，unordered_map 不保证字段排列顺序。
    for (auto &head : rsp._headers) {
        rsp_str << head.first << ": " << head.second << "\r\n";
    }
    // 头部后必须有一个空行；其后才是正文，正文末尾不额外添加 CRLF。
    rsp_str << "\r\n";
    // HEAD 仍输出响应头和计算得到的长度，但不发送正文。
    // 长度是否正确描述对应 GET 内容，要由处理器保证，序列化器不会替业务推算。
    if (req._method != "HEAD") rsp_str << rsp._body;
    // 3. Send 将完整字节串交给连接的输出路径；实际 send 由网络层推进，
    // 这里返回不代表对端已收完，也不等于立刻关闭 socket。
    const std::string response = rsp_str.str();
    conn->Send(response.data(), response.size());
}

// 判断能否使用普通静态文件处理入口；返回 false 时 Route 还会尝试动态路由。
// 这里只查路径/类型，不读取文件内容，也不保证后续打开一定成功。
bool HttpServer::IsFileHandler(const HttpRequest &req) {
    // 1. 必须设置了静态资源根目录
    if (_basedir.empty()) {
        return false;
    }
    // 2. 请求方法，必须是GET / HEAD请求方法
    if (req._method != "GET" && req._method != "HEAD") {
        return false;
    }
    // 3. 使用当前字符串级路径检查，试图拒绝向根目录外跳出的 ..。
    // 注意：ValidPath 对 . 的处理不完整，stat 也会跟随符号链接，不能当成安全沙箱。
    if (Util::ValidPath(req._path) == false) {
        return false;
    }
    // 4. 拼出磁盘路径：例如 /image/a.png → ./wwwroot/image/a.png。
    // 用局部副本，不改变用于动态路由的 req._path；尾部为 / 时补 index.html。
    std::string req_path = _basedir + req._path;//为了避免直接修改请求的资源路径，因此定义一个临时对象
    if (!req._path.empty() && req._path.back() == '/')  {
        req_path += "index.html";
    }
    // 通过 stat 判断是否为普通文件，不把目录、管道等当成页面读取。
    if (Util::IsRegular(req_path) == false) {
        return false;
    }
    return true;
}

// 普通静态响应：在连接 loop 中整份 ReadFile 到内存，不是 M2 的流式下载。
// 由 Route 在 IsFileHandler 通过后调用；有效请求路径非空，才能访问 back()。
void HttpServer::FileHandler(const HttpRequest &req, HttpResponse *rsp) {
    // 使用与 IsFileHandler 一致的路径规则，否则“检查的文件”和“读的文件”会不同。
    std::string req_path = _basedir + req._path;
    if (req._path.back() == '/')  {
        req_path += "index.html";
    }
    // &rsp->_body 是输出字符串的地址，ReadFile 会修改响应正文。
    // 对大文件，这条同步整文件路径会占内存并阻塞 loop，产物下载另用线程池。
    bool ret = Util::ReadFile(req_path, &rsp->_body);
    if (ret == false) {
        // 现有行为：只返回，未把状态改为 404/500；之前 stat 成功不保证读取成功。
        // 这里只解释该限制，不改变错误处理逻辑。
        return;
    }
    // 按后缀推测 MIME，未知后缀兜底为 application/octet-stream。
    std::string mime = Util::ExtMime(req_path);
    rsp->SetHeader("Content-Type", mime);
    return;
}

// 在指定方法的路由表中寻找首个完整匹配的路径，并同步调用其业务回调。
// 本函数是路由分派，不是 EventLoop::Dispatcher 的跨线程投递操作。
void HttpServer::Dispatcher(HttpRequest &req, HttpResponse *rsp, Handlers &handlers) {
    // vector 按注册顺序遍历，路由项中 first 是正则，second 是可调用对象。
    for (auto &handler : handlers) {
        const std::regex &re = handler.first;
        const Handler &functor = handler.second;
        // regex_match 要求整个路径匹配；查询串已被解析器拆到 _params，不参与匹配。
        // 同时保存捕获组，例如 /numbers/(\d+) 匹配 /numbers/12345 时，
        // req._matches[0] 是完整路径，[1] 是 12345，业务通过 .str() 取出字符串。
        bool ret = std::regex_match(req._path, req._matches, re);
        if (ret == false) {
            continue;
        }
        // functor(req, rsp) 此刻就执行并填写响应；return 表示执行后停止遍历。
        // 普通回调运行在连接 loop，不能误认为 std::function 会自动创建后台线程。
        return functor(req, rsp);//传入请求信息，和空的rsp，执行处理函数
    }
    // 方法支持，但此表没有匹配路径：404。这里只改状态，不自动填 HTML 错误页。
    rsp->_statu = 404;
}

// 普通请求分类：静态文件优先，然后选请求方法对应的动态路由表。
// 启用 _async_handler 后，OnMessage 改走异步业务，不调用这个普通入口。
void HttpServer::Route(HttpRequest &req, HttpResponse *rsp) {
    // 文件和动态路由同时适用时，当前实现先返回文件内容。
    if (IsFileHandler(req) == true) {
        //是一个静态资源请求, 则进行静态资源请求的处理
        return FileHandler(req, rsp);
    }
    // HEAD 复用 GET 表，由 WriteReponse 省略正文，而不是要求业务另注册 HEAD 路由。
    if (req._method == "GET" || req._method == "HEAD") {
        return Dispatcher(req, rsp, _get_route);
    }else if (req._method == "POST") {
        return Dispatcher(req, rsp, _post_route);
    }else if (req._method == "PUT") {
        return Dispatcher(req, rsp, _put_route);
    }else if (req._method == "DELETE") {
        return Dispatcher(req, rsp, _delete_route);
    }
    // 只有未支持的方法才到这里：405；普通路径没匹配到则由 Dispatcher 返回 404。
    // 当前 HttpContext 更早也会拒绝未支持方法，因此通常在解析阶段就已报错。
    rsp->_statu = 405;// Method Not Allowed
    return ;
}

// 连接建立后，为它安装独立的解析上下文；网络层用 Any 存储，不必认识 HTTP 类型。
// 只在建立阶段初始化，不是每收到一批数据就新建 Context，否则半包进度会丢失。
void HttpServer::OnConnected(const PtrConnection &conn) {
    conn->SetContext(HttpContext(_max_body_size));
    DBG_LOG("NEW CONNECTION %p", conn.get());
}

// HTTP 消息处理总入口，在连接所属 loop 中读取输入 Buffer 并推进同一个 Context。
// 一次 recv 不等于一份请求：while 可处理 Buffer 中连续的多个普通完整请求；
// 遇半包、读暂停或异步响应等待则 return，成员状态/未消费字节保留到下次调用。
void HttpServer::OnMessage(const PtrConnection &conn, Buffer *buffer) {
    while (true) {
        // 1. 从连接的 Any 取出原 Context，拿到的是指针，不是新建解析器。
        HttpContext *context = conn->GetContext()->get<HttpContext>();
        // 【扩展】Busy 表示上一请求输入已完整，但异步响应尚未结束。
        // ReadPaused 还可能因上传等磁盘或输出背压而成立；此时不能继续推进下一请求。
        if (context->Busy() || conn->ReadPaused()) return;
        // 空 Buffer 通常意味着等新数据，但 BODY 的零长度输入仍可触发最终消费者。
        // 因此不能把“可读字节为 0”一律作为返回条件。
        if (!buffer->ReadAbleSize() && context->RecvStatu() != RECV_HTTP_BODY) return;

        // 2. 先只解析请求行/头（false 表示不消费正文），给预检与流式准备留机会。
        // 即使 Buffer 已经有正文，也先保留，不能在鉴权前聚合整份文件。
        context->RecvHttpRequest(buffer, false);
        // req 引用 Context 内的请求；稍后 ReSet 会清空其内容，后台任务不能长期引用它。
        HttpRequest &req = context->Request();
        // 临时 rsp 保存本轮普通响应或预检拒绝结果，此时仍未发送任何响应。
        HttpResponse rsp(context->RespStatu());
        if (context->RespStatu() >= 400) {
            // 2.1 请求行/头部解析错误：填 HTML，明确关闭，不继续解释剩余输入。
            ErrorHandler(req, &rsp);//填充一个错误显示页面数据到rsp中
            rsp.SetHeader("Connection", "close");
            WriteReponse(conn, req, rsp);//组织响应发送给客户端
            // 先完成序列化，再清除 req；这里和“半包 return”不同，出错需要清理。
            context->ReSet();
            buffer->MoveReadOffset(buffer->ReadAbleSize());//出错了就把缓冲区数据清空
            // Shutdown 停止读取并等待已有输出，实际资源释放由连接状态机处理。
            conn->Shutdown();//关闭连接
            return;
        }

        // 2.2 进入 BODY 说明头已经完整；HeadersChecked 确保本请求只执行一次预检。
        // 未完成的头行还在 Buffer 时不会到这里；分批正文也不会反复取得业务名额。
        if (context->RecvStatu() == RECV_HTTP_BODY && !context->HeadersChecked()) {
            // 【扩展】guard 是头部回调输出的资源票据，例如上传/下载并发名额。
            std::shared_ptr<void> guard;
            // 未注册则跳过；注册且返回 false 时，rsp 应由业务填好错误状态/正文。
            if (_header_handler && !_header_handler(req, &rsp, &guard)) {
                rsp.SetHeader("Connection", "close");
                WriteReponse(conn, req, rsp);
                context->ReSet();
                buffer->MoveReadOffset(buffer->ReadAbleSize());
                conn->Shutdown();
                return;
            }
            // 将票据交给 Context 持有，标记已检查；重置/取消时按引用生命周期归还。
            context->SetGuard(guard);
            context->MarkHeadersChecked();
            // 【扩展】例如产物上传会安装 consumer/cancel 并先暂停读、准备临时文件。
            // 后面的正文解析会调用 consumer；未准备好时消费者拒绝且不消费字节。
            if (_stream_factory) _stream_factory(conn, context);

            // 客户端可能先发头并等待允许，再发大正文；当前只识别精确的 100-continue。
            if (req.HasHeader("Expect")) {
                if (req.GetHeader("Expect") != "100-continue") {
                    // 不支持的期望返回 417；若流式准备已开始，ReSet 会触发取消收尾。
                    rsp._statu = 417;
                    rsp.SetHeader("Connection", "close");
                    ErrorHandler(req, &rsp);
                    WriteReponse(conn, req, rsp);
                    context->ReSet();
                    buffer->MoveReadOffset(buffer->ReadAbleSize());
                    conn->Shutdown();
                    return;
                }
                // 100 是临时响应，不是最终业务成功，不重置 Context。
                const std::string interim = "HTTP/1.1 100 Continue\r\n\r\n";
                // 普通路径直接允许；流式上传由业务在版本/配额/文件准备成功后再发 100。
                if (!context->Streaming()) conn->Send(interim.data(), interim.size());
            }
        }

        // 2.3 再次驱动解析，默认允许正文：普通路径 append，流式路径交给 consumer。
        // 若头仍不完整，此次调用仍只尝试当前阶段，不会跳过状态去误读正文。
        context->RecvHttpRequest(buffer);
        if (context->RespStatu() >= 400) {
            // 正文解析也可能报错（如普通聚合超 8 MiB），必须使用这次更新后的状态码。
            // Reply 完成输出与重置；Connection: close 使它不再恢复后续请求读取。
            HttpResponse error(context->RespStatu());
            ErrorHandler(req, &error);
            error.SetHeader("Connection", "close");
            Reply(conn, req, error);
            return;
        }
        if (context->RecvStatu() != RECV_HTTP_OVER) {
            // 输入不完整或 consumer 暂未接受：只结束当前调用，不 ReSet，不清空 Buffer。
            // 等下一批 socket 数据，或异步准备/写块完成后的 ResumeRead 重新驱动。
            return;
        }

        // 3. 只有输入阶段为 OVER，才进入最终业务分派。OVER 不表示磁盘提交已经完成。
        if (_async_handler) {
            // 【扩展】本请求回复完成前，暂停处理后续请求，避免 HTTP 响应交错。
            context->SetBusy();
            conn->PauseRead(Connection::HttpResponsePending);
            // 回调在当前 loop 立即执行，由其自行提交磁盘任务；不是自动开一个线程。
            // 有此接口就不再走 Route。产物上传在这里不重复回复，最后一块回调负责回复。
            _async_handler(conn, req);
            return;
        }
        
        // 普通路径：静态文件或同步 Handler 填 rsp，调用结束后在本函数统一序列化。
        try { Route(req, &rsp); }
        catch (const std::exception &) {
            // 普通业务抛标准异常时，尽量转为 500 错误页并关闭，而不是当成正常成功。
            ERR_LOG("HTTP handler failed");
            rsp = HttpResponse(500);
            ErrorHandler(req, &rsp);
            rsp.SetHeader("Connection", "close");
        }
        // 4. 根据 req/rsp 生成独立的响应字节串，并交给 Connection 的输出队列。
        WriteReponse(conn, req, rsp);
        // 5. 本请求普通处理结束，重置解析状态；不清 Buffer，里面可能已有下一份请求。
        context->ReSet();
        // 6. WriteReponse 已补 Connection；根据最终响应决定关闭还是继续。
        if (rsp.Close()) {
            // 不再处理已到达的后续请求；等待当前响应输出后关闭连接。
            buffer->MoveReadOffset(buffer->ReadAbleSize());
            conn->Shutdown();
            return;
        }
        // 保持连接：自然进入下一轮。下一请求可能已在 Buffer，不必等新 EPOLLIN。
        // 若无完整数据或输出背压暂停，循环前面的检查会返回，不会忙等。
    }
    return;
}

// 构造底层 TcpServer 并接入 HTTP 回调；此时还没调用 Start 运行事件循环。
// 普通正文上限先设为 8 MiB，产物服务可在 Listen 前另配置更大的流式上限。
HttpServer::HttpServer(int port, int timeout, const std::string &ip):_server(port, ip), _max_body_size(8 * 1024 * 1024) {
    // 配置连接非活跃超时，由底层时间轮管理，不是 HTTP 正文总传输时长限制。
    _server.EnableInactiveRelease(timeout);
    // 成员函数要绑定调用对象 this；_1 是网络层未来传入的 conn，不是现在执行回调。
    _server.SetConnectedCallback(std::bind(&HttpServer::OnConnected, this, std::placeholders::_1));
    // OnMessage 需要两个运行时参数：_1=conn，_2=该连接输入 Buffer 的指针。
    _server.SetMessageCallback(std::bind(&HttpServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2));
    // lambda 是另一种绑定方式：捕获 this，关闭时由底层传 conn 再调用成员函数。
    // this 不自动延长对象寿命，HttpServer 必须在这些回调执行期间保持有效。
    _server.SetClosedCallback([this](const PtrConnection &conn) { OnClosed(conn); });
}

// 关闭回调：撤销该连接未完成请求的业务状态，而非在这里重复 close socket。
// Reset 会触发 Context 的 cancel 回调；磁盘清理由业务安排，不在 loop 上等待它结束。
void HttpServer::OnClosed(const PtrConnection &conn) {
    // 连接可能在 OnConnected 安装上下文之前就关闭，因此先确认 Any 内确实有对象。
    if (conn->GetContext()->HasValue()) conn->GetContext()->get<HttpContext>()->ReSet();
}

// 【扩展】在所属 loop 完成一个非流式响应（如元数据 JSON、上传成功结果）。
// 业务工作线程应经安全 Dispatcher 回到该 loop 再调用，不能直接跨线程重置 Context。
// rsp 按值传入，下面补齐字段只改本地副本；req 在完成序列化前必须仍有效。
void HttpServer::Reply(const PtrConnection &conn, const HttpRequest &req, HttpResponse rsp) {
    WriteReponse(conn, req, rsp);
    // 必须在 WriteReponse 补完 Connection 后判断，不能仅看刚构造的 rsp 默认行为。
    const bool close = rsp.Close();
    // 输出已经取得本请求的版本/关闭策略等数据，可以清请求并通知传输收尾。
    conn->GetContext()->get<HttpContext>()->ReSet();
    if (close) conn->Shutdown();
    else {
        // 只清本路径负责的暂停原因；OutputPressure 等其他原因仍会保留。
        // ResumeRead 会检查连接状态和所有暂停位，并排队再处理已有输入，避免回调重入。
        conn->ResumeRead(Connection::Application);
        conn->ResumeRead(Connection::HttpResponsePending);
    }
}

// 【扩展】开始流式响应：发送头部，正文随后由业务逐块 Send；暂不重置 Context。
// 调用方通常让 rsp._body 为空，明确设置完整文件/范围长度，不能靠空 body 推算长度。
void HttpServer::BeginResponse(const PtrConnection &conn, const HttpRequest &req, HttpResponse rsp) {
    // 先标记正文生产尚未完成：等待磁盘下一块时输出 Buffer 可能暂时为空，
    // 不能因此在 Shutdown 时误认为整个响应结束并提前关闭连接。
    conn->BeginStream();
    WriteReponse(conn, req, rsp);
}

// 【扩展】最后一块已交给输出队列时结束流式生产，不要求对端已经收完。
// 由连接所属 loop 调用，且生产方应已把传输标为完成，避免 Reset 将正常完成当成取消。
void HttpServer::EndResponse(const PtrConnection &conn, bool close) {
    // 不再因输出降到低水位而调用旧传输的 Pump，防止完成后继续安排文件读任务。
    conn->SetLowWaterCallback(nullptr);
    // 清除“仍有正文待生产”的标志；若已处于待关闭且输出为空，底层会安排释放。
    conn->EndStream();
    conn->GetContext()->get<HttpContext>()->ReSet();
    // 关闭时等待已排队输出；保持连接时只解除响应等待位，其他暂停原因仍有效。
    // 即使还有前一响应尾部，后一响应也只能追加在后面，保持字节顺序。
    if (close) conn->Shutdown();
    else conn->ResumeRead(Connection::HttpResponsePending);
}

// 设置普通静态文件根目录（相对路径以进程工作目录为基准），不启动文件服务线程。
void HttpServer::SetBaseDir(const std::string &path) {
    // assert 只提供断言检查，启用 NDEBUG 的构建可能将它移除，不是可靠的运行时校验。
    // 目录存在也不代表整个静态路径处理已经成为安全沙箱。
    assert(Util::IsDirectory(path) == true);
    _basedir = path;
}

// 注册 GET 路由，也供 HEAD 选择；此刻编译正则并保存回调，不立即调用 handler。
// 无效正则可能在注册时抛异常；请求到达时 Dispatcher 才按顺序匹配、执行。
void HttpServer::Get(const std::string &pattern, const Handler &handler) {
    _get_route.push_back(std::make_pair(std::regex(pattern), handler));
}

// 注册 POST 路由；这里只按方法分类，业务含义由 handler 决定，不自动实现登录/上传。
void HttpServer::Post(const std::string &pattern, const Handler &handler) {
    _post_route.push_back(std::make_pair(std::regex(pattern), handler));
}

// 注册 PUT 路由；框架不因为方法叫 PUT 就自动写文件，写入逻辑由业务回调提供。
void HttpServer::Put(const std::string &pattern, const Handler &handler) {
    _put_route.push_back(std::make_pair(std::regex(pattern), handler));
}

// 注册 DELETE 路由；此函数本身不删除任何资源，只登记路径规则和业务处理器。
void HttpServer::Delete(const std::string &pattern, const Handler &handler) {
    _delete_route.push_back(std::make_pair(std::regex(pattern), handler));
}

// 转交底层设置网络工作 loop 数量；一个 loop 可管理多连接，不是“一线程一连接”。
// 应在 Listen 前配置；这也不是产物服务的 DiskExecutor 磁盘线程数量。
void HttpServer::SetThreadCount(int count) {
    _server.SetThreadCount(count);
}

// 真正启动 TCP 服务和 EventLoop；连接/消息/关闭发生时，才执行已经注册好的回调。
// 通常阻塞运行直到停止；若要 signalfd 停机，应事先调用 EnableSignalStop。
void HttpServer::Listen() {
    _server.Start();
}
