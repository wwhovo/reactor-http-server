#pragma once
#include "HttpContext.hpp"
#include "HttpResponse.hpp"
#include "../net/TcpServer.hpp"
#include <functional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#define DEFALT_TIMEOUT 10 // 默认连接非活跃超时，单位为秒；沿用原宏名拼写。

// TCP 网络层之上的 HTTP 调度器：组合 TcpServer，而不是重新实现 epoll/recv/send。
// 每条连接保存自己的 HttpContext；本类负责驱动解析、选择业务、序列化响应。
// 普通路径：OnMessage → HttpContext → Route → Handler → WriteReponse。
// 异步路径：头部预检/安装消费者 → 输入完整 → AsyncHandler → Reply 或 Begin/EndResponse。
// 学习时先看普通路径，再看下面标为【扩展】的接口。
// 配置与路由注册应在 Listen 前完成；运行期间跨线程修改它们没有额外同步保护。
class HttpServer {
    private:
        // 普通业务回调：读取请求，通过 rsp 指针填写结果；本身不必调用 Send。
        // std::function 可以保存普通函数、lambda 或签名兼容的 bind 对象。
        using Handler = std::function<void(const HttpRequest &, HttpResponse *)>;
        // 路由表的每项是“路径正则 + 业务函数”，vector 保留注册顺序，先匹配者先处理。
        using Handlers = std::vector<std::pair<std::regex, Handler>>;
        // 按请求方法分表；HEAD 与 GET 共享 _get_route，不另设 HEAD 表。
        Handlers _get_route;
        Handlers _post_route;
        Handlers _put_route;
        Handlers _delete_route;
        // 普通静态资源根目录，如 ./wwwroot；为空时不启用该文件处理入口。
        std::string _basedir; //静态资源根目录
        // 底层服务器管理监听、连接和 EventLoop；本类通过注册回调接入它。
        TcpServer _server;
        // 新建 HttpContext 时传入的声明正文上限，普通服务默认 8 MiB。
        size_t _max_body_size;

        // 【扩展】头部预检：头完整后、消费正文前执行。
        // 返回 true 放行；false 拒绝，并由回调填写 rsp（例如 401/413/429）。
        // 第三个参数输出不透明资源票据，交给 Context 持有，绑定请求资源的生命周期。
        using HeaderHandler = std::function<bool(const HttpRequest &, HttpResponse *, std::shared_ptr<void> *)>;
        HeaderHandler _header_handler;
        // 【扩展】通过预检后配置本请求，可安装 Context 的正文消费者/取消回调。
        // 它是同步调用的“准备接口”，具体业务可自行提交后台准备任务、暂停读取。
        using StreamFactory = std::function<void(const PtrConnection &, HttpContext *)>;
        StreamFactory _stream_factory;
        // 【扩展】输入完整时交给异步业务，取代普通 Route 分支。
        // 回调仍在连接所属 loop 中被调用；后台工作和完成投递由业务自己安排。
        std::function<void(const PtrConnection &, const HttpRequest &)> _async_handler;
    private:
        // 构造本层错误 HTML 正文；不发送，也不在这里决定关闭连接。
        void ErrorHandler(const HttpRequest &req, HttpResponse *rsp);
        // 补响应头，将 rsp 序列化成 HTTP 字节，再交给 Connection::Send。
        // 名字沿用 WriteReponse 的现有拼写；HEAD 在此省略正文发送。
        void WriteReponse(const PtrConnection &conn, const HttpRequest &req, HttpResponse &rsp);
        // 检查是否适用普通静态文件入口：配置根目录、GET/HEAD、路径检查、普通文件。
        bool IsFileHandler(const HttpRequest &req);
        // 静态文件整份读取到 rsp->_body，再设置 MIME；不是产物服务的大文件流式路径。
        void FileHandler(const HttpRequest &req, HttpResponse *rsp);
        // 遍历选中的路由表，填 req._matches 并调用首个匹配回调，无匹配则 404。
        // 这里的 Dispatcher 是路由分派，不是 EventLoop 的安全跨线程投递句柄。
        void Dispatcher(HttpRequest &req, HttpResponse *rsp, Handlers &handlers);
        // 普通同步业务入口：先试静态文件，再按方法选择一张表交给 Dispatcher。
        void Route(HttpRequest &req, HttpResponse *rsp);
        // 连接建立回调：通过 Any 给该 Connection 安装独立的 HttpContext。
        void OnConnected(const PtrConnection &conn);
        // 消息回调：消费输入 Buffer，保留半包进度，完成请求后路由/回复或交异步业务。
        void OnMessage(const PtrConnection &conn, Buffer *buffer);
        // 连接关闭回调：重置请求，通知未完成传输取消，释放 Context 持有的资源。
        void OnClosed(const PtrConnection &conn);
    public:
        // 创建底层 TCP 服务，设置非活跃超时并注册上述三个回调；并未开始事件循环。
        // 默认 ip=0.0.0.0 表示监听所有 IPv4 网卡；timeout 是非活跃连接的秒数。
        HttpServer(int port, int timeout = DEFALT_TIMEOUT, const std::string &ip = "0.0.0.0");
        // 设置后续 OnConnected 创建 Context 时使用的上限，不追溯修改已有 Context。
        // 普通聚合正文仍受 HttpContext 内部 8 MiB 限制；更大正文须走流式消费者。
        void SetMaxBodySize(size_t bytes) { _max_body_size = bytes; }
        // 【扩展】注册回调，不立即执行；实际执行点在 OnMessage 的头部完整阶段。
        void SetHeaderHandler(const HeaderHandler &handler) { _header_handler = handler; }
        // 【扩展】注册流式准备接口；解析正文前安装消费者才能避免整份正文聚合。
        void SetStreamFactory(const StreamFactory &f) { _stream_factory = f; }
        // 【扩展】启用异步业务路径；业务最终需调用回复/结束接口，恢复请求处理。
        void SetAsyncHandler(const std::function<void(const PtrConnection &, const HttpRequest &)> &f) { _async_handler = f; }

        // 【扩展】在连接所属 loop 中发送完整的小响应，重置请求并关闭或恢复读取。
        // rsp 按值接收，补头不修改调用者的响应；req 必须仍有效，后台先复制所需数据。
        void Reply(const PtrConnection &conn, const HttpRequest &req, HttpResponse rsp);
        // 【扩展】在所属 loop 开始一个流式响应：标记生产未结束，并发送响应头。
        // 调用者通常将 rsp 正文留空、显式设置完整 Content-Length，随后逐块 conn->Send。
        void BeginResponse(const PtrConnection &conn, const HttpRequest &req, HttpResponse rsp);
        // 【扩展】最后一块已入发送队列后，在所属 loop 结束流、重置请求。
        // close 是是否关闭连接的决定；结束生产不等于对端已经接收全部字节。
        void EndResponse(const PtrConnection &conn, bool close);

        // 启用底层 signalfd 停机机制；应在启动工作线程之前调用，普通 Listen 不自动启用。
        void EnableSignalStop() { _server.EnableSignalStop(); }
        // 停止接入新连接，给已有连接宽限时间；不是后台磁盘任务的退出硬时限。
        void Stop(int grace_seconds = 3) { _server.Stop(grace_seconds); }
        // 获取实际绑定端口；配置为 0 时可取得系统分配的端口。
        uint16_t Port() { return _server.Port(); }
        // 配置普通静态资源根目录；当前使用 assert 检查目录，Release 下可能不执行检查。
        void SetBaseDir(const std::string &path);

        // 以下接口只注册“路径正则 → 普通回调”，不立刻处理请求。
        // Get 也供 HEAD 使用；Post/Put/Delete 分别登记到对应方法表。
        void Get(const std::string &pattern, const Handler &handler);
        void Post(const std::string &pattern, const Handler &handler);
        void Put(const std::string &pattern, const Handler &handler);
        void Delete(const std::string &pattern, const Handler &handler);
        // 配置底层工作 EventLoop 线程数，不是“每连接一个线程”；0 时由主 loop 管连接。
        void SetThreadCount(int count);
        // 启动底层监听与事件循环；通常直到服务器停止后才返回，不是注册回调的接口。
        void Listen();
};
