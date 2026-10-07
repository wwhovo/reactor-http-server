# Reactor HTTP Server

基于 **C++11 与 Linux epoll** 的多线程 HTTP 服务器，采用主从 Reactor 架构，将事件分发、连接管理、协议解析和业务路由分层组织。

项目提供静态资源访问、正则路由、HEAD 响应和 HTTP 长连接处理，并通过 CMake/CTest 管理构建与自动化验证。网络层可独立用于 TCP 服务。

## 核心实现

- **主从 Reactor 与线程归属**：主 EventLoop 接收新连接，按轮询策略分配给工作 EventLoop；每条连接的 I/O 与协议处理由所属线程执行，一个工作线程可管理多条连接。
- **跨线程任务投递**：使用任务队列和互斥锁接收操作，通过 eventfd 唤醒事件循环；区分同线程立即执行与排队执行，减少跨线程直接修改连接状态。
- **HTTP 增量解析**：以请求行、头部、正文、完成和错误状态推进解析；保留未消费字节与解析进度，处理分段输入及同一连接上的连续请求。
- **非阻塞收发与缓冲管理**：分离输入/输出 Buffer，按实际 recv/send 字节数推进偏移；有待发数据时启用 EPOLLOUT，发送完毕后取消写监控，处理部分发送与 EAGAIN。
- **连接生命周期与资源回收**：使用 shared_ptr 管理连接，延后执行实际释放；基于 timerfd 与时间轮维护非活跃超时，网络层提供信号驱动停机与有限宽限时间。
- **HTTP 路由与响应组织**：按请求方法选择路由表，正则完整匹配路径并提取捕获组；静态资源优先，匹配首个动态路由后调用处理器，再统一序列化响应。

## 处理架构

```text
客户端
  │ 新连接
  ▼
主 EventLoop：Acceptor → TcpServer → 轮询选择工作 EventLoop
  │
  ▼
工作 EventLoop：epoll_wait → Channel → Connection
  │                                        │
  │                                  输入 Buffer
  │                                        ▼
  │                              HttpContext 增量解析
  │                                        ▼
  │                          HttpServer → 静态文件 / 路由处理器
  │                                        ▼
  │                                  HttpResponse
  │                                        ▼
  └── 可写事件 → HandleWrite ← 输出 Buffer ← 响应序列化
                     │
                     ▼
                    send
```

工作线程数为 0 时，连接由主 EventLoop 管理。跨线程操作回到目标 loop 后执行；响应入队不代表客户端已经接收，关闭请求会等待已排队输出完成。

## 核心目录

| 目录 | 职责 |
| --- | --- |
| `source/base/` | 类型擦除容器 Any、日志与网络初始化 |
| `source/net/` | Socket/Buffer、Channel/Poller、EventLoop、线程池、时间轮及 TCP 连接管理 |
| `source/http/` | HTTP 请求/响应、URL 与文件工具、状态机解析、路由及服务入口 |
| `source/echo/` | TCP 回显服务入口 |
| `tests/` | 组件检查、协议边界及端到端验证 |

主要入口：[HTTP 服务](source/http/main.cc)、[HTTP 调度](source/http/HttpServer.cc)、[解析状态机](source/http/HttpContext.cc)、[TCP 服务器](source/net/TcpServer.cc)。

## 构建与运行

环境要求：Linux、支持 C++11 的编译器、CMake（最低配置版本 3.10）与 pthread。执行测试还需要 Python 3。

从仓库根目录构建：

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j2
```

HTTP 服务默认监听 `127.0.0.1:8085`，配置 3 个工作线程。静态资源使用相对目录 `./wwwroot/`，因此从以下目录启动：

```sh
cd source/http
../../build-debug/http_server
```

在另一终端验证动态路由、静态资源、HEAD 和未匹配路径：

```sh
curl -i 'http://127.0.0.1:8085/hello?name=alice'
curl -i -d 'x=1' http://127.0.0.1:8085/login
curl -i http://127.0.0.1:8085/index.html
curl -I http://127.0.0.1:8085/index.html
curl -i http://127.0.0.1:8085/not-found
```

`/hello` 与 POST `/login` 返回请求信息的文本表示，后者不是账号认证接口；`/index.html` 返回静态页面，未匹配路径返回 404。HEAD 发送响应头但不发送正文，保留对应 GET 内容长度。

使用 Ctrl+C 或 SIGTERM 正常停止 HTTP 服务并回收工作线程。默认配置只允许本机访问；若修改监听地址进行网络演示，应另行评估访问控制和传输安全。

TCP 回显服务从仓库根目录用 `./build-debug/echo_server` 启动，默认端口 8500。

## 自动化验证

```sh
ctest --test-dir build-debug --output-on-failure
```

用例涉及多翻译单元链接、正文长度与溢出、HTTP 分段解析与流水线请求、非法报文、HEAD、长连接、静态文件路径/读取错误，以及网络半关闭、重复释放、RST 与停机。

已记录的 Debug 验证结果（2026-10-07）：

| 构建配置 | 结果 |
| --- | --- |
| Debug | 六项全部通过，连续两轮 |
| Release | 六项全部通过 |
| Debug + ASan/UBSan | 六项全部通过，已执行用例未报告检查器错误 |

六项为 `http_components`、`network_components`、`network_integration`、`http_boundaries`、`echo_smoke`、`http_smoke`。测试串行执行，端到端用例会使用本机 8085 和 8500 端口，请确保端口未被其他服务占用。权限拒绝用例在 root 用户下会跳过，本次以普通用户执行。

ASan/UBSan 检查命令：

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-sanitize -j2
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-sanitize --output-on-failure
```

检查结论只覆盖已执行用例，不是长期稳定性或生产可用性证明。Echo 示例在 smoke 结束时直接终止，不能据此声称验证了它的正常退出泄漏检查。

## 实现边界

- HTTP 为协议子集：支持 HTTP/1.0、HTTP/1.1 的当前请求处理路径及 GET、HEAD、POST、PUT、DELETE 路由；不提供 TLS 或 Transfer-Encoding 解析，特殊状态码响应处理仍需完善。
- 静态文件整份同步读取，可能阻塞连接所属 loop 并占用较多内存。真实路径检查会拒绝根目录外的符号链接目标，但检查与打开之间仍存在竞态，不构成完整安全沙箱；静态目录应受控且不允许不可信用户修改。读取失败返回 500。
- 演示入口未注册 PUT/DELETE 处理器，不提供文件管理或认证功能；框架保留相应方法的路由注册接口。当前仅适用于受控演示，不直接暴露到不可信网络。
- 尚未完成可复现性能测试，不提供未经测量的 QPS、延迟或并发能力结论。
