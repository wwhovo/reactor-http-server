#include "Connection.hpp"
#include <stdexcept>

void Connection::HandleRead() {
    if (_statu != CONNECTED || ReadPaused()) return;
    //1. 接收socket的数据，放到缓冲区
    char buf[65536];
    ssize_t ret = _socket.NonBlockRecv(buf, 65535);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        return Release();
    }
    if (ret == 0) return ShutdownInLoop(); // 对端关闭写方向，仍可把响应发完。
    //将数据放入输入缓冲区,写入之后顺便将写偏移向后移动
    _in_buffer.WriteAndPush(buf, ret);
    //2. 调用message_callback进行业务处理
    if (_in_buffer.ReadAbleSize() > 0 && _message_callback) {
        //shared_from_this--从当前对象自身获取自身的shared_ptr管理对象
        return _message_callback(shared_from_this(), &_in_buffer);
    }
}

void Connection::HandleWrite() {
    if (_statu == DISCONNECTED) return;
    //_out_buffer中保存的数据就是要发送的数据
    ssize_t ret = _socket.NonBlockSend(_out_buffer.ReadPosition(), _out_buffer.ReadAbleSize());
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        return Release();
    }
    _out_buffer.MoveReadOffset(ret);//千万不要忘了，将读偏移向后移动
    const size_t pending = _out_buffer.ReadAbleSize();
    const bool notify = (_above_high && pending <= _low_water) || pending == 0;
    if (_above_high && pending <= _low_water) {
        _above_high = false;
        ResumeRead(OutputPressure);
    }
    if (_out_buffer.ReadAbleSize() == 0) {
        _channel.DisableWrite();// 没有数据待发送了，关闭写事件监控
        //如果当前是连接待关闭状态，则有数据，发送完数据释放连接，没有数据则直接释放
    }
    if (notify && _low_water_callback) {
        auto callback = _low_water_callback;
        callback(shared_from_this());
    }
    if (_statu == DISCONNECTING && !_streaming_output && _out_buffer.ReadAbleSize() == 0) Release();
    return;
}

void Connection::HandleClose() {
    ShutdownInLoop();
}

void Connection::HandleError() {
    Release();
}

void Connection::HandleEvent() {
    if (_statu == DISCONNECTED) return;
    if (_enable_inactive_release == true)  {  _loop->TimerRefresh(_conn_id); }
    if (_event_callback)  {  _event_callback(shared_from_this()); }
}

void Connection::EstablishedInLoop() {
    // 1. 修改连接状态；  2. 启动读事件监控；  3. 调用回调函数
    if (_statu != CONNECTING) return;
    _statu = CONNECTED;//当前函数执行完毕，则连接进入已完成连接状态
    // 一旦启动读事件监控就有可能会立即触发读事件，如果这时候启动了非活跃连接销毁
    _channel.EnableRead();
    if (_connected_callback) _connected_callback(shared_from_this());
}

void Connection::ReleaseInLoop() {
    _loop->AssertInLoop();
    if (_statu == DISCONNECTED) return; // 多个错误/挂断事件只能关闭一次。
    //1. 修改连接状态，将其置为DISCONNECTED
    _statu = DISCONNECTED;
    _streaming_output = false;
    _low_water_callback = AnyEventCallback();
    //2. 移除连接的事件监控
    _channel.Remove();
    //3. 关闭描述符
    _socket.Close();
    //4. 如果当前定时器队列中还有定时销毁任务，则取消任务
    if (_loop->HasTimer(_conn_id)) CancelInactiveReleaseInLoop();
    //5. 调用关闭回调函数，避免先移除服务器管理的连接信息导致Connection被释放，再去处理会出错，因此先调用用户的回调函数
    if (_closed_callback) _closed_callback(shared_from_this());
    //移除服务器内部管理的连接信息
    if (_server_closed_callback) _server_closed_callback(shared_from_this());
}

void Connection::SendInLoop(Buffer &buf) {
    if (!StreamWritable()) return;
    if (_out_buffer.ReadAbleSize() + buf.ReadAbleSize() > _output_limit) {
        ERR_LOG("connection output buffer limit exceeded");
        Release();
        return;
    }
    _out_buffer.WriteBufferAndPush(buf);
    _peak_output = std::max(_peak_output, static_cast<size_t>(_out_buffer.ReadAbleSize()));
    if (_out_buffer.ReadAbleSize() >= _high_water) { _above_high = true; PauseRead(OutputPressure); }
    if (_channel.WriteAble() == false) {
        _channel.EnableWrite();
    }
}

void Connection::ShutdownInLoop() {
    if (_statu == DISCONNECTED || _statu == DISCONNECTING) return;
    _statu = DISCONNECTING;// 设置连接为半关闭状态
    _channel.DisableRead();
    //要么就是写入数据的时候出错关闭，要么就是没有待发送数据，直接关闭
    if (_out_buffer.ReadAbleSize() > 0) {
        if (_channel.WriteAble() == false) {
            _channel.EnableWrite();
        }
    }
    if (_out_buffer.ReadAbleSize() == 0 && !_streaming_output) {
        Release();
    }
}

void Connection::EnableInactiveReleaseInLoop(int sec) {
    //1. 将判断标志 _enable_inactive_release 置为true
    _enable_inactive_release = true;
    //2. 如果当前定时销毁任务已经存在，那就刷新延迟一下即可
    if (_loop->HasTimer(_conn_id)) {
        return _loop->TimerRefresh(_conn_id);
    }
    //3. 如果不存在定时销毁任务，则新增
    _loop->TimerAdd(_conn_id, sec, std::bind(&Connection::Release, this));
}

void Connection::CancelInactiveReleaseInLoop() {
    _enable_inactive_release = false;
    if (_loop->HasTimer(_conn_id)) { 
        _loop->TimerCancel(_conn_id); 
    }
}

void Connection::UpgradeInLoop(const Any &context, 
            const ConnectedCallback &conn, 
            const MessageCallback &msg, 
            const ClosedCallback &closed, 
            const AnyEventCallback &event) {
    _context = context;
    _connected_callback = conn;
    _message_callback = msg;
    _closed_callback = closed;
    _event_callback = event;
}

Connection::Connection(EventLoop *loop, uint64_t conn_id, int sockfd):_conn_id(conn_id), _sockfd(sockfd),
    _enable_inactive_release(false), _loop(loop), _statu(CONNECTING), _socket(_sockfd),
    _channel(loop, _sockfd), _read_pauses(0), _streaming_output(false), _above_high(false),
    _low_water(128 * 1024), _high_water(256 * 1024), _output_limit(32 * 1024 * 1024), _peak_output(0) {
    _channel.SetCloseCallback(std::bind(&Connection::HandleClose, this));
    _channel.SetEventCallback(std::bind(&Connection::HandleEvent, this));
    _channel.SetReadCallback(std::bind(&Connection::HandleRead, this));
    _channel.SetWriteCallback(std::bind(&Connection::HandleWrite, this));
    _channel.SetErrorCallback(std::bind(&Connection::HandleError, this));
}

Connection::~Connection() { DBG_LOG("RELEASE CONNECTION:%p", this); }

int Connection::Fd() { return _socket.Fd(); }

uint64_t Connection::Id() { return _conn_id; }

bool Connection::Connected() { return (_statu == CONNECTED); }

void Connection::PauseRead(uint32_t reason) {
    _loop->AssertInLoop();
    _read_pauses |= reason;
    if (_statu != DISCONNECTED && _channel.ReadAble()) _channel.DisableRead();
}

void Connection::ResumeRead(uint32_t reason) {
    _loop->AssertInLoop();
    _read_pauses &= ~reason;
    if (_statu != CONNECTED || ReadPaused()) return;
    if (!_channel.ReadAble()) _channel.EnableRead();
    auto self = shared_from_this();
    // 排队处理已读入的字节（包括零长度正文），避免 HTTP 回调重入。
    _loop->QueueInLoop([self]() {
        if (self->Connected() && !self->ReadPaused() && self->_message_callback)
            self->_message_callback(self, &self->_in_buffer);
    });
}

void Connection::SetOutputWatermarks(size_t low, size_t high, size_t limit) {
    _loop->AssertInLoop();
    if (low >= high || high > limit) throw std::invalid_argument("输出水位必须满足 low < high <= limit");
    _low_water = low; _high_water = high; _output_limit = limit;
}

void Connection::SetLowWaterCallback(const AnyEventCallback &callback) {
    _loop->AssertInLoop(); _low_water_callback = callback;
}

void Connection::BeginStream() {
    _loop->AssertInLoop();
    if (_streaming_output) throw std::logic_error("同一连接不能交错发送两个流");
    _streaming_output = true;
}

void Connection::EndStream() {
    _loop->AssertInLoop();
    _streaming_output = false;
    if (_statu == DISCONNECTING && _out_buffer.ReadAbleSize() == 0) Release();
}

void Connection::SetContext(const Any &context) { _context = context; }

Any *Connection::GetContext() { return &_context; }

void Connection::SetConnectedCallback(const ConnectedCallback&cb) { _connected_callback = cb; }

void Connection::SetMessageCallback(const MessageCallback&cb) { _message_callback = cb; }

void Connection::SetClosedCallback(const ClosedCallback&cb) { _closed_callback = cb; }

void Connection::SetAnyEventCallback(const AnyEventCallback&cb) { _event_callback = cb; }

void Connection::SetSrvClosedCallback(const ClosedCallback&cb) { _server_closed_callback = cb; }

void Connection::Established() {
    auto self = shared_from_this();
    _loop->RunInLoop([self]() { self->EstablishedInLoop(); });
}

void Connection::Send(const char *data, size_t len) {
    //外界传入的data，可能是个临时的空间，我们现在只是把发送操作压入了任务池，有可能并没有被立即执行
    //因此有可能执行的时候，data指向的空间有可能已经被释放了。
    Buffer buf;
    buf.WriteAndPush(data, len);
    auto self = shared_from_this();
    _loop->RunInLoop([self, buf]() mutable { self->SendInLoop(buf); });
}

void Connection::Shutdown() {
    auto self = shared_from_this();
    _loop->RunInLoop([self]() { self->ShutdownInLoop(); });
}

void Connection::Release() {
    auto self = shared_from_this();
    _loop->QueueInLoop([self]() { self->ReleaseInLoop(); });
}

void Connection::EnableInactiveRelease(int sec) {
    if (sec < 1 || sec > 60) throw std::invalid_argument("空闲超时必须在 1~60 秒内");
    auto self = shared_from_this();
    _loop->RunInLoop([self, sec]() { if (self->_statu != DISCONNECTED) self->EnableInactiveReleaseInLoop(sec); });
}

void Connection::CancelInactiveRelease() {
    auto self = shared_from_this();
    _loop->RunInLoop([self]() { self->CancelInactiveReleaseInLoop(); });
}

void Connection::Upgrade(const Any &context, const ConnectedCallback &conn, const MessageCallback &msg, 
             const ClosedCallback &closed, const AnyEventCallback &event) {
    _loop->AssertInLoop();
    _loop->RunInLoop(std::bind(&Connection::UpgradeInLoop, this, context, conn, msg, closed, event));
}
