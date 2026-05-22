#include "services/real_realtime_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>  // 当前代码里使用了 std::vector<uint8_t>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/system/system_error.hpp>
#include <openssl/ssl.h>

#include "common/config.h"
#include "common/logger.h"
#include "common/protocol.h"

namespace interview::services {
    
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;

using tcp = asio::ip::tcp;

// WssStream 表示一个 WSS 连接：
// WebSocket 层
//   -> SSL/TLS 层
//      -> TCP 层
//
// 也就是：WebSocket over TLS over TCP。
using WssStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

// 默认实时对话接口路径。
// 如果配置里的 base_url 只有 wss://host，或者 path 是 "/"，就使用这个默认路径。
constexpr const char* kDefaultTarget = "/api/v3/realtime/dialogue";

// Close 阶段等待服务端返回结束事件的最长时间。
// 例如等待 kSessionFinished / kConnectionFinished。
constexpr std::chrono::seconds kCloseEventTimeout(3);

// Connect 阶段等待服务端返回启动事件的最长时间。
// 例如等待 kConnectionStarted / kSessionStarted。
constexpr std::chrono::seconds kConnectEventTimeout(10);

/**
 * 拆解 WSS URL 后得到的结果。
 *
 * 示例：
 *   wss://openspeech.bytedance.com/api/v3/realtime/dialogue
 *
 * 解析后：
 *   host   = "openspeech.bytedance.com"
 *   port   = "443"
 *   target = "/api/v3/realtime/dialogue"
 */
struct UrlParts {
    std::string host;
    std::string port = "443";
    std::string target = kDefaultTarget;
};


/**
 * 解析 wss:// URL。
 *
 * 当前只支持 wss://，不支持 ws://、http://、https://。
 *
 * 支持的格式：
 *   wss://host
 *   wss://host/
 *   wss://host/path
 *   wss://host:port/path
 *
 * 注意：
 *   这里用 rfind(':') 解析端口，适合普通域名。
 *   如果后续支持 IPv6 地址，比如 wss://[::1]:443/path，
 *   这段解析逻辑需要增强。
 */
UrlParts ParseWssUrl(const std::string& url) {
    constexpr const char* kScheme = "wss://";
    constexpr std::size_t kSchemeLen = 6;

    // 检查协议前缀。
    // 不以 wss:// 开头就直接报错。
    if (url.compare(0, kSchemeLen, kScheme) != 0) {
        throw std::runtime_error("only wss:// realtime urls are supported");
    }

    // authority_begin 指向 wss:// 后面的第一个字符。
    //
    // 例如：
    //   wss://example.com/path
    //         ^
    //         authority_begin
    const std::size_t authority_begin = kSchemeLen;
    //
    const std::size_t path_begin = url.find('/', authority_begin);

    // authority 是host 或 host:port.
    // 例如：
    //   wss://example.com:443/path
    // authority = "example.com:443"
    const std::string authority = 
        path_begin == std::string::npos 
                ? url.substr(authority_begin)
                : url.substr(authority_begin, path_begin - authority_begin);

    //  host 部分不能为空
    if (authority.empty()) {
        throw std::runtime_error("wss url host is empty");
    }

    UrlParts parts;

    // 尝试解析端口。
    //
    // 例如：
    //   authority = "example.com:8443"
    //
    // 解析后：
    //   host = "example.com"
    //   port = "8443"
    //
    // 如果没有端口，就使用默认的 443。
    const std::size_t colon = authority.rfind(":");
    if (colon != std::string::npos && colon + 1 < authority.size()) {
        parts.host = authority.substr(0, colon);
        parts.port = authority.substr(colon + 1);
    } else {
        parts.host = authority;
    }

    // 如果 URL 里带了 path，就使用 URL 中的 path。
    //
    // 例如：
    //   wss://example.com/ws
    //
    // target = "/ws"
    if (path_begin != std::string::npos && path_begin < url.size()) {
        parts.target = url.substr(path_begin);
    }
    // 如果path为空，或者只是"/",  就用默认路径
    if (parts.target.empty() || parts.target == "/") {
        parts.target = kDefaultTarget;
    } 
    // 再次检查 host 是否为空。
    // 比如 wss://:443/path 这类情况可能导致 host 为空。
    if (parts.host.empty()) {
        throw std::runtime_error("wss url host is empty");
    }
    return parts;
}


/**
 * 构造 WebSocket 握手时使用的 Host header。
 *
 * 规则：
 *   默认 443 端口：
 *     Host: example.com
 *
 *   非默认端口：
 *     Host: example.com:8443
 */
std::string HostHeader(const UrlParts& parts) {
    if (parts.port == "443") {
        return parts.host;
    }
    return parts.host + ":" + parts.port;
}

bool IsRetryableIoError(const beast::error_code& ec) {
    return ec == asio::error::try_again ||
           ec == asio::error::would_block;
}

bool IsTransportClosedError(const beast::error_code& ec) {
    return ec == websocket::error::closed ||
           ec == asio::error::operation_aborted ||
           ec == asio::error::eof ||
           ec == asio::error::bad_descriptor ||
           ec == asio::error::connection_reset ||
           ec == ssl::error::stream_truncated ||
           ec == ssl::error::unspecified_system_error;
}

// StartSession 是会话级请求，V1 二进制协议要求帧体中必须有
// [session_id_size 4B] + [session_id bytes]，然后才是
// [payload_size 4B] + [payload bytes]。
//
// StartConnection 仍然是连接级请求，不携带 session_id；但一旦进入
// StartSession，客户端需要先生成一个本轮会话 ID 写入请求帧。否则服务端
// 会把 payload_size 错读成 session_id_size，随后继续读 payload_size 时
// 就会出现 "parse payload size failed: body too short"。
std::string GenerateSessionId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> hex_digit(0, 15);
    std::uniform_int_distribution<int> uuid_variant(8, 11);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    // UUID v4 text form: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx.
    // Version is fixed to 4; variant y must be one of 8, 9, a, b.
    for (int i = 0; i < 8; ++i) {
        oss << hex_digit(gen);
    }
    oss << '-';
    for (int i = 0; i < 4; ++i) {
        oss << hex_digit(gen);
    }
    oss << "-4";
    for (int i = 0; i < 3; ++i) {
        oss << hex_digit(gen);
    }
    oss << '-';
    oss << uuid_variant(gen);
    for (int i = 0; i < 3; ++i) {
        oss << hex_digit(gen);
    }
    oss << '-';
    for (int i = 0; i < 12; ++i) {
        oss << hex_digit(gen);
    }

    return oss.str();
}

}
// Impl：阶段 5 占位实现
// 阶段 7 这里会接入 boost::asio::io_context / ssl::context / websocket::stream
//        + recv_thread_ + std::atomic<bool> running_ + std::mutex send_mutex_
class RealRealtimeClient::Impl {
public:
    using EventHandler = RealtimeClient::EventHandler;

    explicit Impl(std::string server_url)
        : server_url_(std::move(server_url))
        , url_(ParseWssUrl(server_url_))
        , ssl_context_(ssl::context::tlsv12_client) {}

    ~Impl() {
        Close();
    }

    /**
     * 建立真实 WSS 连接。
     *
     * 整体流程：
     *   1. 如果已经连接，直接返回 true；
     *   2. 清理旧状态；
     *   3. 设置 TLS 证书校验；
     *   4. DNS 解析；
     *   5. 建立 TCP 连接；
     *   6. 设置 TLS SNI；
     *   7. 设置 WebSocket 鉴权 headers；
     *   8. TLS 握手；
     *   9. WebSocket 握手；
     *   10. 启动接收线程；
     *   11. 发送 StartConnection，等待 kConnectionStarted；
     *   12. 发送 StartSession，等待 kSessionStarted；
     *   13. 返回 true。
     */
    bool Connect() {
        // 如果已经处于 connected 状态，就不重复连接。
        if (connected_.load()) {
            return false;
        }

        try {
            if (!JoinReceiveThread()) {
                throw std::runtime_error("Connect cannot run on receive thread");
            }
            ws_.reset();
            ioc_.restart();

            closing_.store(false);
            failed_.store(false);

            // 清空上一轮连接留下来的事情和 session_id.
            {
                std::lock_guard<std::mutex> lock(event_mutex_);
                seen_events_.clear();
                session_id_.clear();
            }
            // 使用系统默认 CA证书路径。
            // verity_peer 表示校验服务端证书
            ssl_context_.set_default_verify_paths();
            ssl_context_.set_verify_mode(ssl::verify_peer);

            // DNS 解析：把host + port 解析成可连接的IP 地址。
            tcp::resolver resolver(ioc_);
            auto const results = resolver.resolve(url_.host, url_.port);

            // 创建WSS stream
            ws_ = std::make_unique<WssStream>(ioc_, ssl_context_);

            // 给底层TCP 连接设置 30秒超时
            beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));
            // 建立 TCP 连接
            beast::get_lowest_layer(*ws_).connect(results);

            // 设置TLS SNI
            // 
            // SNI = Server Name Indication
            // 多个域名可能共用同一个 IP，客户端需要告诉服务端自己要访问哪个域名。
            // 这一步必须在 TLS handshake 之前做。
            if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(),
                                          url_.host.c_str())) {
                throw std::runtime_error("failed to set TLS SNI host");
            }
            // 从全局 Config单例中读取WebSocket 鉴权 headers
            // 从 Snapshot() 返回配置副本，避免外部直接访问内部 config
            const auto headers = interview::common::Config::Instance().Snapshot().ws.headers;
            // WebSocket decorator 用于在握手请求里添加自定义请求头
            // 按值捕获 headers:
            ws_->set_option(websocket::stream_base::decorator(
                [headers](websocket::request_type& req){
                    req.set("User-Agent", "AI-Interview/1.0");
                    if (!headers.api_app_id.empty()) {
                        req.set("X-Api-App-ID", headers.api_app_id);
                    }
                    if (!headers.api_access_key.empty()) {
                        req.set("X-Api-Access-Key", headers.api_access_key);
                    }
                    if (!headers.api_resource_id.empty()) {
                        req.set("X-Api-Resource-Id", headers.api_resource_id);
                    }
                    if (!headers.api_app_key.empty()) {
                        req.set("X-Api-App-Key", headers.api_app_key);
                    }
                    if (!headers.api_connect_id.empty()) {
                        req.set("X-Api-Connect-Id", headers.api_connect_id);
                    }
            }));
            // TLS 握手
            // 注意：必须先完成TLS 握手，才能进行WebSocket 握手。
            ws_->next_layer().handshake(ssl::stream_base::client);

            // WebSocket握手
            ws_->handshake(HostHeader(url_), url_.target);

            // 设置WebSocket 后续发送二进制帧。
            // 因为 Protocol::BuildFullRequest / BuildClientAudioRequest
            // 生成的是二进制协议帧
            ws_->binary(true);

            // TLS/TCP 连接完成后， 取消底层TCP 的临时连接超时
            beast::get_lowest_layer(*ws_).expires_never();
            // 设置 Beast 推荐的WebSocket 客户端超时配置
            ws_->set_option(websocket::stream_base::timeout::suggested(
                beast::role_type::client));
            
            connected_.store(true);
            running_.store(true);

            // 启动后天接收线程
            // 这个线程会持续 ws_->read(), 收到服务端事件后调用NotifyEvent
            recv_thread_ = std::thread(&Impl::ReceiveLoop, this);
            
            // 发送StartConnection事件
            // 还未创建session，session_id 为空
            if (!SendEvent(interview::common::events::kStartConnection, "",
                            nlohmann::json::object())) {
                throw std::runtime_error("failed to send StartConnection");
            }
            // 等待服务端返回kConnectionStarted
            if (!WaitForEvent(interview::common::events::kConnectionStarted,
                                kConnectEventTimeout)) {
                throw std::runtime_error(
                    "timeout waiting for kConnectionStarted");
            }

            const std::string start_session_id = GenerateSessionId();
            {
                std::lock_guard<std::mutex> lock(event_mutex_);
                session_id_ = start_session_id;
            }
            LOG_INFO("[RealRealtimeClient] starting session session_id={}",
                     start_session_id);

            // 构造StartSession payload
            const nlohmann::json payload = 
                        interview::common::Config::Instance().BuildStartSessionPayload();

            // 发送StartSession事件
            if (!SendEvent(interview::common::events::kStartSession, start_session_id, payload)) {
                throw std::runtime_error("failed to send StartSession");
            }

            // 等待服务端返回kSessionStarted
            if (!WaitForEvent(interview::common::events::kSessionStarted, kConnectEventTimeout)) {
                throw std::runtime_error("timeout waiting for kSessionStarted");
            }

            LOG_INFO("[RealRealtimeClient] connected to {}{}", url_.host, url_.target);
            return true;
        } catch (std::exception& e) {
            // Startup failures are local transport failures, not a completed
            // protocol session. Do not send FinishSession/FinishConnection here.
            LOG_ERROR("[RealRealtimeClient] Connect failed: {}", e.what());
            failed_.store(true);
            FinishLocalClose();
            return false;
        }
    }
    /**
     * Closes the client.
     *
     * Normal owner-thread shutdown sends protocol FinishSession / FinishConnection
     * best-effort, then performs local transport cleanup. Startup failures and
     * receive-thread initiated shutdown skip protocol close because they cannot
     * safely wait for acknowledgement frames.
     */
    void Close() {
        if (closing_.exchange(true)) {
            FinishLocalClose();
            return;
        }

        const bool on_receive_thread = IsReceiveThread();
        const bool was_connected = connected_.load();
        const bool can_send_protocol_close =
            was_connected && !failed_.load() && !on_receive_thread;

        if (can_send_protocol_close) {
            std::string sid;
            {
                std::lock_guard<std::mutex> lock(event_mutex_);
                sid = session_id_;
            }

            if (!sid.empty()) {
                ForgetEvent(interview::common::events::kSessionFinished);
                (void)SendEvent(interview::common::events::kFinishSession, sid,
                                nlohmann::json::object());
                (void)WaitForEvent(interview::common::events::kSessionFinished,
                                   kCloseEventTimeout);
            }

            ForgetEvent(interview::common::events::kConnectionFinished);
            (void)SendEvent(interview::common::events::kFinishConnection, "",
                            nlohmann::json::object());
            (void)WaitForEvent(interview::common::events::kConnectionFinished,
                               kCloseEventTimeout);
        } else if (on_receive_thread) {
            // The receive thread cannot wait for protocol close acknowledgements
            // that only the receive thread itself could read.
            LOG_WARN("[RealRealtimeClient] Close called on receive thread; skipping protocol close");
        }

        FinishLocalClose();
        LOG_INFO("[RealRealtimeClient] closed");
    }

    // 发送普通业务事件
    bool SendEvent(uint32_t event,
        const std::string& session_id,
        const nlohmann::json& payload) {
        if (!connected_.load() || !ws_) {
            LOG_WARN("[RealRealtimeClient] SendEvent(event = {}) on closed client",
                event);
            return false;
        }
        try {
            // 把event + session_id + payload 打包成二进制协议帧
            const std::vector<uint8_t> frame = 
                    interview::common::Protocol::BuildFullRequest(
                            event, session_id, payload);
            if (!WriteFrame(frame, "event")) {
                return false;
            }

            if (event == interview::common::events::kChatTextQuery) {
                LOG_INFO("[RealRealtimeClient] sent ChatTextQuery event={} session_id={} payload_bytes={}",
                         event, session_id, payload.dump().size());
            } else {
                LOG_DEBUG("[RealRealtimeClient] send event={}, session_id = {}", event, session_id);
            }
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("[RealRealtimeClient] SendEvent failed: {}", e.what());
            return false;
        }
    }
    /**
     * 发送音频数据。
     *
     * 参数：
     *   session_id 当前会话 ID
     *   pcm        PCM 音频字节流
     *
     * 当前事件固定为 kTaskRequest。
     * 后续 Stage 8 录音线程会持续调用这个函数发送用户语音。
     */
    bool SendAudio(const std::string& session_id,
                   const std::vector<uint8_t>& pcm) {
        // 没连接时不能发送音频。
        if (!connected_.load() || !ws_) {
            LOG_WARN("[RealRealtimeClient] SendAudio on closed client");
            return false;
        }

        try {
            // 把 PCM 音频封装成协议帧。
            const std::vector<uint8_t> frame =
                interview::common::Protocol::BuildClientAudioRequest(
                    interview::common::events::kTaskRequest, session_id, pcm);

            if (!WriteFrame(frame, "audio")) {
                return false;
            }

            LOG_DEBUG("[RealRealtimeClient] sent audio bytes={}", pcm.size());
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("[RealRealtimeClient] SendAudio failed: {}", e.what());
            return false;
        }
    }

    void SetEventHandler(EventHandler handler) {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler_ = std::move(handler);
    }

    bool IsConnected() const {
        return connected_.load();
    }   
    
private:
    bool WriteFrame(const std::vector<uint8_t>& frame, const char* kind) {
        std::lock_guard<std::mutex> lock(send_mutex_);

        for (int attempt = 0; attempt < 2; ++attempt) {
            beast::error_code ec;
            ws_->binary(true);
            ws_->write(asio::buffer(frame), ec);
            if (!ec) {
                return true;
            }

            if (IsRetryableIoError(ec) && attempt == 0) {
                LOG_DEBUG("[RealRealtimeClient] {} write would block; retrying",
                          kind);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (IsTransportClosedError(ec)) {
                LOG_WARN("[RealRealtimeClient] {} write on closed transport: {}",
                         kind, ec.message());
                running_.store(false);
                connected_.store(false);
            } else {
                LOG_ERROR("[RealRealtimeClient] {} write failed: {}",
                          kind, ec.message());
                failed_.store(true);
            }
            event_cv_.notify_all();
            return false;
        }

        return false;
    }

    /**
     * 后台接收线程主循环。
     *
     * 工作内容：
     *   1. 阻塞读取 WebSocket 消息；
     *   2. 把 Beast buffer 转成字节数组；
     *   3. 调用 Protocol::ParseResponse 解析协议帧；
     *   4. NotifyEvent 通知等待线程；
     *   5. 调用业务事件回调 handler。
     */
    void ReceiveLoop() {
        while (running_.load()) {
            try {
                beast::flat_buffer buffer;
                // 阻塞读取一条 WebSocket消息。
                // 没有消息时， 这里会阻塞
                ws_->read(buffer);

                if (!ws_->got_binary()) {
                    const std::string text =
                        beast::buffers_to_string(buffer.data());
                    LOG_WARN("[RealRealtimeClient] ignored non-binary frame "
                             "bytes={}",
                             text.size());
                    continue;
                }

                // 把Beast buffer 转成string
                const std::string raw = beast::buffers_to_string(buffer.data());
                // 把string 转成 vector<uin8_t>， 交给协议解析函数
                std::vector<uint8_t> bytes(raw.begin(), raw.end());
                // 解析服务端返回的二进制协议帧
                auto parsed = interview::common::Protocol::ParseResponse(bytes);

                NotifyEvent(parsed);

                EventHandler handler;
                {
                    std::lock_guard<std::mutex> lock(handler_mutex_);
                    handler = handler_;
                }

                if (handler) {
                    handler(parsed);
                }
            } catch (const boost::system::system_error& e) {
                const beast::error_code ec = e.code();
                if (IsRetryableIoError(ec)) {
                    LOG_DEBUG("[RealRealtimeClient] receive would block; retrying");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                if (ec == websocket::error::closed &&
                    (closing_.load() || !running_.load())) {
                    LOG_INFO("[RealRealtimeClient] peer closed WebSocket");
                } else if (ec == websocket::error::closed) {
                    LOG_WARN("[RealRealtimeClient] peer closed WebSocket");
                    NotifyConnectionFailure("peer closed WebSocket");
                } else if (IsTransportClosedError(ec) &&
                           (closing_.load() || !running_.load())) {
                    LOG_DEBUG("[RealRealtimeClient] receive stopped: {}",
                              ec.message());
                } else if (IsTransportClosedError(ec)) {
                    LOG_WARN("[RealRealtimeClient] transport closed by peer: {}",
                             ec.message());
                    NotifyConnectionFailure("transport closed by peer: " +
                                            ec.message());
                } else {
                    LOG_ERROR("[RealRealtimeClient] receive loop failed: {}",
                              e.what());
                    failed_.store(true);
                    NotifyConnectionFailure("receive loop failed: " +
                                            std::string(e.what()));
                }

                event_cv_.notify_all();
                break;
            } catch (const std::exception& e) {
                // 如果 running_ 仍然为 true，说明不是主动关闭导致的异常。
                if (running_.load()) {
                    LOG_ERROR("[RealRealtimeClient] receive loop failed: {}",
                              e.what());

                    // 标记连接失败，唤醒等待中的 WaitForEvent。
                    failed_.store(true);
                    NotifyConnectionFailure("receive loop failed: " +
                                            std::string(e.what()));
                    event_cv_.notify_all();
                }

                break;
            }
        }
        running_.store(false);
        connected_.store(false);
        event_cv_.notify_all();
    }

    void NotifyConnectionFailure(const std::string& reason) {
        if (closing_.load()) {
            return;
        }

        interview::common::ParsedResponse evt;
        evt.message_type = interview::common::MessageType::kServerFullResponse;
        evt.event = interview::common::events::kConnectionFailed;
        evt.payload_json = reason;

        NotifyEvent(evt);

        EventHandler handler;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler = handler_;
        }
        if (handler) {
            handler(evt);
        }
    }
    /**
     * 记录收到的服务端事件，并唤醒等待线程。
     *
     * 例如：
     *   Connect() 中等待 kConnectionStarted；
     *   接收线程收到 kConnectionStarted 后调用 NotifyEvent；
     *   NotifyEvent 插入 seen_events_ 并 notify_all；
     *   WaitForEvent 被唤醒。
     */
    void NotifyEvent(const interview::common::ParsedResponse& evt) {
        {
            std::lock_guard<std::mutex> lock(event_mutex_);

            // 记录已经收到过的事件 ID。
            seen_events_.insert(evt.event);
            // 如果响应里带 session_id，就保存下来。
            // 后续 FinishSession / SendAudio 会用到。
            if (!evt.session_id.empty()) {
                session_id_ = evt.session_id;
            }

            if (evt.event == interview::common::events::kConnectionFailed ||
                evt.event == interview::common::events::kSessionFailed) {
                    failed_.store(true);
            }
        }
        event_cv_.notify_all();
    }
    /**
     * 等待指定事件出现。
     *
     * 返回 true 的条件：
     *   1. 在 timeout 内收到了目标 event；
     *   2. 没有 failed_。
     *
     * 返回 false 的情况：
     *   1. 超时；
     *   2. 收到失败事件；
     *   3. running_ 变成 false。
     */
    bool WaitForEvent(uint32_t event, std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(event_mutex_);
        const bool ready = event_cv_.wait_for(lock, timeout, [&] {
            return seen_events_.count(event) != 0 ||
                    failed_.load() ||
                    !running_.load();
        });
        return ready && seen_events_.count(event) != 0 && !failed_.load();
    }

    /**
     * 清除某个已收到事件。
     *
     * Close 阶段会用它清掉旧的 kSessionFinished / kConnectionFinished，
     * 避免误认为“本次关闭已经收到确认”。
     */
    void ForgetEvent(uint32_t event) {
        std::lock_guard<std::mutex> lock(event_mutex_);
        seen_events_.erase(event);
    }

    void FinishLocalClose() {
        running_.store(false);
        connected_.store(false);
        event_cv_.notify_all();

        CloseTransport();
        ioc_.stop();

        if (JoinReceiveThread()) {
            ws_.reset();
            ioc_.restart();
        }
    }


    void CloseTransport() {
        if (!ws_) {
            return;
        }

        // The receive thread may already have consumed a WebSocket close frame
        // from the server. Calling websocket::close() after that state trips a
        // Beast debug assertion, so local shutdown uses the lowest TCP layer to
        // release the blocking read and then joins the thread.
        std::lock_guard<std::mutex> lock(send_mutex_);
        beast::error_code ec;

        beast::get_lowest_layer(*ws_).socket().cancel(ec);
        if (ec) {
            LOG_DEBUG("[RealRealtimeClient] transport cancel ignored: {}",
                      ec.message());
        }

        ec = {};
        beast::get_lowest_layer(*ws_).socket().shutdown(
            tcp::socket::shutdown_both, ec);
        if (ec && ec != asio::error::not_connected) {
            LOG_DEBUG("[RealRealtimeClient] transport shutdown ignored: {}",
                      ec.message());
        }

        ec = {};
        beast::get_lowest_layer(*ws_).socket().close(ec);
        if (ec) {
            LOG_DEBUG("[RealRealtimeClient] transport close ignored: {}",
                      ec.message());
        }
    }
    bool IsReceiveThread() const {
        return recv_thread_.joinable() &&
               recv_thread_.get_id() == std::this_thread::get_id();
    }

    /**
     * Waits for the receive thread to finish.
     *
     * Returns false when called from the receive thread itself. In that case
     * the owner thread must call Close() later to join and release ws_.
     */
    bool JoinReceiveThread() {
        if (!recv_thread_.joinable()) {
            return true;
        }
        if (IsReceiveThread()) {
            LOG_WARN("[RealRealtimeClient] receive thread join deferred");
            return false;
        }
        recv_thread_.join();
        return true;
    }

private:
    // 例如：wss://openspeech.bytedance.com/api/v3/realtime/dialogue
    std::string server_url_;
    // 解析后的 host/ port / target
    UrlParts url_;
    // Boost.Asio 的IO 上下文
    asio::io_context ioc_;
    // TLS 客户端上下文
    ssl::context ssl_context_;
    // WSS连接对象
    std::unique_ptr<WssStream> ws_;
    // 保护WebSocket 写操作
    mutable std::mutex send_mutex_;
    // 保护 handler_
    std::mutex handler_mutex_;
    // 业务层注册的事件回调
    EventHandler handler_;
    // 保护seen_events 和session_id
    std::mutex event_mutex_;
    // 用于 WaitForEvent 等待接收线程通知
    std::condition_variable event_cv_;
    // 已经收到过的事件合集
    std::set<uint32_t> seen_events_;
    // 当前session ID
    std::string session_id_;

    std::thread recv_thread_;
    // 接收线程是否继续运行
    std::atomic<bool> running_{false};
    // 当前连接是否处于已连接状态
    std::atomic<bool> connected_{false};
    // 当前是否正在关闭
    std::atomic<bool> closing_{false};
    // 当前连接或session 是否失败
    std::atomic<bool> failed_{false};
};


// ===== 外壳方法（仅做 impl_ 转发） =====

RealRealtimeClient::RealRealtimeClient(std::string server_url)
    : impl_(std::make_unique<Impl>(std::move(server_url))) {}

// 必须在 .cc 里定义析构：unique_ptr<Impl> 需要 Impl 完整类型
// 写在头里会因为 Impl 是 forward declare 而编译失败
RealRealtimeClient::~RealRealtimeClient() = default;

bool RealRealtimeClient::Connect() {
    return impl_->Connect();
}
void RealRealtimeClient::Close() {
    impl_->Close();
}

bool RealRealtimeClient::SendEvent(uint32_t event,
    const std::string& session_id,
    const nlohmann::json& payload) {
    return impl_->SendEvent(event, session_id, payload);
}

bool RealRealtimeClient::SendAudio(const std::string& session_id,
    const std::vector<uint8_t>& pcm) {
    return impl_->SendAudio(session_id, pcm);
}

void RealRealtimeClient::SetEventHandler(EventHandler handler) {
    impl_->SetEventHandler(std::move(handler));
}

bool RealRealtimeClient::IsConnected() const {
    return impl_->IsConnected();
}


}
