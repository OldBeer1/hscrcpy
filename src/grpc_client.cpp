#include "grpc_client.h"
#include "net_util.h"
#include "log.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

#ifdef _WIN32
using socket_t = SOCKET;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCK (-1)
#define CLOSE_SOCKET close
#endif

namespace hscrcpy {

static bool wsa_initialized = false;

static void init_wsa() {
#ifdef _WIN32
    if (!wsa_initialized) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        wsa_initialized = true;
    }
#endif
}

GrpcClient::GrpcClient() : sock_(-1) { init_wsa(); }
GrpcClient::~GrpcClient() { disconnect(); }

// ============================================================
// Socket helpers
// ============================================================

static bool send_all(int sock, const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = ::send(static_cast<socket_t>(sock),
                       reinterpret_cast<const char *>(data + sent),
                       static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static int recv_with_timeout(int sock, uint8_t *buf, size_t len, int timeout_sec) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(static_cast<socket_t>(sock), &readfds);
    struct timeval tv = {timeout_sec, 0};
    int sel = select(static_cast<int>(sock) + 1, &readfds, nullptr, nullptr, &tv);
    if (sel <= 0) return sel;
    return ::recv(static_cast<socket_t>(sock), reinterpret_cast<char *>(buf),
                  static_cast<int>(len), 0);
}

// ============================================================
// Zero-copy H264 extraction from gRPC protobuf
// ============================================================

static bool extract_h264_ref_from_payload_map(const uint8_t *map_data, size_t map_len, H264Ref &out) {
    size_t pos = 0;
    while (pos < map_len) {
        uint8_t tag = map_data[pos++];
        uint8_t fnum = tag >> 3;
        uint8_t wtype = tag & 0x07;

        auto read_varint = [&]() -> uint64_t {
            uint64_t val = 0;
            uint32_t shift = 0;
            while (pos < map_len) {
                uint8_t b = map_data[pos++];
                val |= static_cast<uint64_t>(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
            return val;
        };

        if (wtype == 2) {
            uint64_t len = read_varint();
            const uint8_t *field_data = map_data + pos;
            pos += len;

            if (fnum == 2) {
                size_t vp = 0;
                while (vp < len) {
                    uint8_t vtag = field_data[vp++];
                    uint8_t vfnum = vtag >> 3;
                    uint8_t vwtype = vtag & 0x07;
                    if (vwtype == 0) {
                        uint64_t vval = 0; uint32_t shift = 0;
                        while (vp < len) { uint8_t b = field_data[vp++]; vval |= (uint64_t)(b & 0x7F) << shift; shift += 7; if (!(b & 0x80)) break; }
                    } else if (vwtype == 2) {
                        uint64_t vlen = 0; uint32_t shift = 0;
                        while (vp < len) { uint8_t b = field_data[vp++]; vlen |= (uint64_t)(b & 0x7F) << shift; shift += 7; if (!(b & 0x80)) break; }
                        if (vfnum == 5 && vp + vlen <= len) {
                            out.data = field_data + vp;
                            out.size = static_cast<size_t>(vlen);
                            return true;
                        }
                        vp += vlen;
                    } else if (vwtype == 1) { vp += 8; }
                    else if (vwtype == 5) { vp += 4; }
                    else break;
                }
            }
        } else if (wtype == 0) {
            read_varint();
        } else if (wtype == 1) { pos += 8; }
        else if (wtype == 5) { pos += 4; }
        else break;
    }
    return false;
}

static bool extract_h264_ref_from_reply(const uint8_t *pb, size_t pb_len, H264Ref &out) {
    size_t pos = 0;
    while (pos < pb_len) {
        uint8_t tag = pb[pos++];
        uint8_t field_num = tag >> 3;
        uint8_t wire_type = tag & 0x07;

        auto read_varint = [&]() -> uint64_t {
            uint64_t val = 0;
            uint32_t shift = 0;
            while (pos < pb_len) {
                uint8_t b = pb[pos++];
                val |= static_cast<uint64_t>(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
            return val;
        };

        if (wire_type == 2) {
            uint64_t len = read_varint();
            const uint8_t *field_data = pb + pos;
            pos += len;

            if (field_num == 3) {
                if (extract_h264_ref_from_payload_map(field_data, static_cast<size_t>(len), out)) {
                    return true;
                }
            }
        } else if (wire_type == 0) {
            read_varint();
        } else if (wire_type == 1) { pos += 8; }
        else if (wire_type == 5) { pos += 4; }
        else break;
    }
    return false;
}

// ============================================================
// gRPC empty payload data provider (5 bytes: 0,0,0,0,0)
// ============================================================

static nghttp2_ssize grpc_empty_read_callback(nghttp2_session *, int32_t,
                                                uint8_t *buf, size_t length,
                                                uint32_t *data_flags,
                                                nghttp2_data_source *, void *) {
    static const uint8_t grpc_empty[5] = {0, 0, 0, 0, 0};
    if (length < 5) return NGHTTP2_ERR_DEFERRED;
    memcpy(buf, grpc_empty, 5);
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return 5;
}

// ============================================================
// Helper: build nghttp2_nv
// ============================================================

static nghttp2_nv make_nv(const char *name, const char *value) {
    return {
        reinterpret_cast<uint8_t *>(const_cast<char *>(name)),
        reinterpret_cast<uint8_t *>(const_cast<char *>(value)),
        strlen(name), strlen(value), NGHTTP2_NV_FLAG_NONE
    };
}

// ============================================================
// gRPC parser reset
// ============================================================

void GrpcClient::reset_grpc_parser() {
    grpc_buf_size_ = 0;
}

// ============================================================
// nghttp2 callbacks — only submit, never flush/send
// ============================================================

int GrpcClient::on_begin_headers_callback(nghttp2_session *, const nghttp2_frame *frame,
                                            void *user_data) {
    auto *self = static_cast<GrpcClient *>(user_data);
    if (frame->hd.stream_id == self->active_stream_id_) {
        self->got_response_headers_ = true;
    }
    return 0;
}

int GrpcClient::on_header_callback(nghttp2_session *, const nghttp2_frame *frame,
                                     const uint8_t *name, size_t namelen,
                                     const uint8_t *value, size_t valuelen,
                                     uint8_t, void *user_data) {
    auto *self = static_cast<GrpcClient *>(user_data);
    if (frame->hd.stream_id == self->active_stream_id_ &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) &&
        namelen == 11 && memcmp(name, "grpc-status", 11) == 0) {
        LOGI("gRPC trailers: grpc-status=%.*s", (int)valuelen, value);
    }
    return 0;
}

int GrpcClient::on_data_chunk_recv_callback(nghttp2_session *, uint8_t,
                                              int32_t stream_id,
                                              const uint8_t *data, size_t len,
                                              void *user_data) {
    auto *self = static_cast<GrpcClient *>(user_data);

    if (stream_id != self->active_stream_id_) {
        return 0;
    }

    const uint8_t *parse_ptr;
    size_t parse_len;

    if (self->grpc_buf_size_ > 0) {
        // Have leftover from previous chunk — append and parse from buffer
        size_t needed = self->grpc_buf_size_ + len;
        if (needed > self->grpc_msg_buf_.size()) {
            self->grpc_msg_buf_.resize(needed);
        }
        memcpy(self->grpc_msg_buf_.data() + self->grpc_buf_size_, data, len);
        self->grpc_buf_size_ = needed;
        parse_ptr = self->grpc_msg_buf_.data();
        parse_len = self->grpc_buf_size_;
    } else {
        // No leftover — zero-copy parse directly from chunk data
        parse_ptr = data;
        parse_len = len;
    }

    // Parse gRPC messages: 5-byte header (1 flag + 4 length) + protobuf payload
    size_t pos = 0;
    while (true) {
        size_t avail = parse_len - pos;
        if (avail < 5) break;

        const uint8_t *hdr = parse_ptr + pos;
        uint32_t msg_len = (static_cast<uint32_t>(hdr[1]) << 24) |
                           (static_cast<uint32_t>(hdr[2]) << 16) |
                           (static_cast<uint32_t>(hdr[3]) << 8) |
                           static_cast<uint32_t>(hdr[4]);
        size_t needed = 5u + msg_len;
        if (avail < needed) break;

        H264Ref h264_ref;
        if (extract_h264_ref_from_reply(parse_ptr + pos + 5, msg_len, h264_ref)) {
            if (h264_ref.size > 0 && self->on_frame_) {
                self->on_frame_(h264_ref.data, h264_ref.size);
                self->got_first_data_ = true;
                self->end_stream_retries_ = 0;
                self->frame_count_++;
            }
        }

        pos += needed;
    }

    // Handle incomplete tail
    size_t remaining = parse_len - pos;
    if (remaining > 0) {
        if (parse_ptr == data) {
            // Was zero-copy — copy tail to buffer
            if (self->grpc_msg_buf_.size() < remaining) {
                self->grpc_msg_buf_.resize(remaining);
            }
            memcpy(self->grpc_msg_buf_.data(), data + pos, remaining);
        } else if (pos > 0) {
            // Was parsing from buffer — compact tail
            memmove(self->grpc_msg_buf_.data(), self->grpc_msg_buf_.data() + pos, remaining);
        }
        self->grpc_buf_size_ = remaining;
    } else {
        self->grpc_buf_size_ = 0;
    }

    return 0;
}

int GrpcClient::on_stream_close_callback(nghttp2_session *, int32_t stream_id,
                                           uint32_t error_code, void *user_data) {
    auto *self = static_cast<GrpcClient *>(user_data);

    if (stream_id == self->active_stream_id_) {
        LOGE("Active stream %d closed (error=%u, retries=%d)",
             stream_id, error_code, self->end_stream_retries_);

        static constexpr int kMaxRetries = 500;
        if (self->end_stream_retries_ < kMaxRetries) {
            self->end_stream_retries_++;
            self->submit_onstart_request();
            LOGE("Reopened stream %d after close", self->active_stream_id_);
        } else {
            LOGW("Max END_STREAM retries reached, stopping streaming");
            self->streaming_ = false;
        }
    } else if (stream_id == self->last_idr_stream_id_) {
        // IDR request stream got RST_STREAM
        self->consecutive_rst_on_idr_++;
        if (self->consecutive_rst_on_idr_ <= 2) {
            LOGI("RST_STREAM on IDR stream %d, retrying (%d/2)", stream_id, self->consecutive_rst_on_idr_);
            self->idr_pending_.store(true);
        } else {
            LOGW("RST_STREAM on IDR stream %d, %d consecutive failures — server does not support IDR requests",
                 stream_id, self->consecutive_rst_on_idr_);
        }
    }

    return 0;
}

int GrpcClient::on_frame_recv_callback(nghttp2_session *, const nghttp2_frame *frame,
                                         void *user_data) {
    auto *self = static_cast<GrpcClient *>(user_data);

    switch (frame->hd.type) {
    case NGHTTP2_SETTINGS:
        // Any SETTINGS frame means handshake progress (server's initial or ACK)
        self->settings_received_ = true;
        break;
    case NGHTTP2_PING:
        if (frame->hd.flags & NGHTTP2_FLAG_ACK) {
            self->ping_fail_count_.store(0);
            self->ping_ack_received_ = true;  // Signal that connection is alive
        }
        break;
    case NGHTTP2_GOAWAY:
        LOGW("Received GOAWAY");
        self->streaming_ = false;
        break;
    }

    return 0;
}

// ============================================================
// Flush nghttp2 pending output to socket
// Uses mem_send2 (no send callback) — coalesce into one send_all
// ============================================================

bool GrpcClient::flush_nghttp2() {
    const uint8_t *data = nullptr;
    // Accumulate all chunks into one buffer, then send once
    // This avoids multiple small ::send() calls and leverages TCP batching
    std::vector<uint8_t> buf;
    while (true) {
        nghttp2_ssize n = nghttp2_session_mem_send2(session_, &data);
        if (n <= 0) break;
        buf.insert(buf.end(), data, data + n);
    }
    if (!buf.empty()) {
        return send_all(sock_, buf.data(), buf.size());
    }
    return true;
}

// ============================================================
// Submit gRPC requests via nghttp2 (queue only, caller must flush)
// ============================================================

bool GrpcClient::submit_onstart_request() {
    if (!session_) return false;

    std::string authority = connected_host_ + ":" + std::to_string(connected_port_);

    nghttp2_nv hdrs[] = {
        make_nv(":method", "POST"),
        make_nv(":path", "/ScrcpyService/onStart"),
        make_nv(":scheme", "http"),
        make_nv(":authority", authority.c_str()),
        make_nv("content-type", "application/grpc"),
        make_nv("te", "trailers"),
        make_nv("grpc-accept-encoding", "gzip"),
        make_nv("user-agent", "hscrcpy-nghttp2/1.0"),
    };

    nghttp2_data_provider2 dp;
    dp.read_callback = grpc_empty_read_callback;

    int32_t sid = nghttp2_submit_request2(session_, nullptr, hdrs,
                                            sizeof(hdrs) / sizeof(hdrs[0]),
                                            &dp, nullptr);
    if (sid < 0) {
        LOGE("nghttp2_submit_request2(onStart) failed: %s", nghttp2_strerror(sid));
        return false;
    }

    active_stream_id_ = sid;
    reset_grpc_parser();
    got_response_headers_ = false;
    consecutive_rst_on_idr_ = 0;
    LOGI("Submitted gRPC onStart (stream %d)", sid);
    return true;
}

bool GrpcClient::submit_idr_request() {
    if (!session_) return false;

    std::string authority = connected_host_ + ":" + std::to_string(connected_port_);

    nghttp2_nv hdrs[] = {
        make_nv(":method", "POST"),
        make_nv(":path", "/ScrcpyService/onRequestIDRFrame"),
        make_nv(":scheme", "http"),
        make_nv(":authority", authority.c_str()),
        make_nv("content-type", "application/grpc"),
        make_nv("te", "trailers"),
        make_nv("grpc-accept-encoding", "gzip"),
        make_nv("user-agent", "hscrcpy-nghttp2/1.0"),
    };

    nghttp2_data_provider2 dp;
    dp.read_callback = grpc_empty_read_callback;

    int32_t sid = nghttp2_submit_request2(session_, nullptr, hdrs,
                                            sizeof(hdrs) / sizeof(hdrs[0]),
                                            &dp, nullptr);
    if (sid < 0) {
        LOGE("nghttp2_submit_request2(IDR) failed: %s", nghttp2_strerror(sid));
        return false;
    }

    last_idr_stream_id_ = sid;
    LOGI("Submitted gRPC onRequestIDRFrame (stream %d)", sid);
    return true;
}

// ============================================================
// connect() — TCP connect + nghttp2 handshake
// ============================================================

bool GrpcClient::connect(const std::string &host, uint16_t port) {
    if (connected_) disconnect();

    connected_host_ = host;
    connected_port_ = port;
    LOGI("GrpcClient::connect(%s, %u) starting", host.c_str(), port);

    // TCP connect
    sock_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (sock_ == static_cast<int>(INVALID_SOCK)) {
        LOGE("socket creation failed");
        return false;
    }

    int flag = 1;
    setsockopt(static_cast<socket_t>(sock_), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char *>(&flag), sizeof(flag));
    set_tcp_keepalive(sock_);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(static_cast<socket_t>(sock_),
                  reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        int err = WSAGetLastError();
        LOGE("connect to %s:%u failed (wsa error %d)", host.c_str(), port, err);
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }
    LOGI("TCP connected to %s:%u", host.c_str(), port);

    // Create nghttp2 session with callbacks
    nghttp2_session_callbacks *cbs;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv_callback);

    // No send callback — we use nghttp2_session_mem_send2() exclusively

    // Use nghttp2 option to enable auto window update (default behavior)
    nghttp2_option *opt;
    nghttp2_option_new(&opt);
    // Don't set no_auto_window_update — let nghttp2 manage WINDOW_UPDATE automatically

    int ret = nghttp2_session_client_new2(&session_, cbs, this, opt);
    nghttp2_option_del(opt);
    nghttp2_session_callbacks_del(cbs);

    if (ret != 0) {
        LOGE("nghttp2_session_client_new2 failed: %s", nghttp2_strerror(ret));
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }

    // Submit client SETTINGS: 4MB window (matching old hand-rolled version), 100 concurrent streams, 64KB max frame
    nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 4194304},
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
        {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, 65536},
    };
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings,
                             sizeof(settings) / sizeof(settings[0]));

    // Send connection preface (24 bytes) + SETTINGS frame
    if (!flush_nghttp2()) {
        LOGE("Failed to send HTTP/2 connection preface");
        nghttp2_session_del(session_);
        session_ = nullptr;
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }

    // Wait for server SETTINGS (confirms HTTP/2 handshake complete)
    settings_received_ = false;
    std::vector<uint8_t> handshake_buf(64 * 1024);
    size_t handshake_used = 0;

    int attempts = 0;
    while (attempts < 10 && !settings_received_) {
        int n = recv_with_timeout(sock_, handshake_buf.data() + handshake_used,
                                  handshake_buf.size() - handshake_used, 2);
        if (n <= 0) {
            attempts++;
            continue;
        }
        handshake_used += n;

        nghttp2_ssize processed = nghttp2_session_mem_recv2(session_,
            handshake_buf.data(), handshake_used);
        if (processed < 0) {
            LOGE("nghttp2_session_mem_recv2 error: %s", nghttp2_strerror(static_cast<int>(processed)));
            nghttp2_session_del(session_);
            session_ = nullptr;
            CLOSE_SOCKET(static_cast<socket_t>(sock_));
            sock_ = -1;
            return false;
        }

        size_t consumed = static_cast<size_t>(processed);
        if (consumed < handshake_used) {
            memmove(handshake_buf.data(), handshake_buf.data() + consumed, handshake_used - consumed);
        }
        handshake_used -= consumed;

        // Flush any pending output (SETTINGS ACK, etc.)
        flush_nghttp2();
    }

    if (!settings_received_) {
        LOGE("Never received server SETTINGS");
        nghttp2_session_del(session_);
        session_ = nullptr;
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }

    // Increase connection-level flow control window to match stream-level (4MB).
    // Default HTTP/2 connection window is 65535 — too small for streaming.
    // Without this, the server stalls after every ~65KB waiting for WINDOW_UPDATE.
    int rv = nghttp2_submit_window_update(session_, NGHTTP2_FLAG_NONE, 0, 4194304 - 65535);
    if (rv == 0) {
        flush_nghttp2();
        LOGI("Connection window increased to 4MB");
    } else {
        LOGW("Failed to increase connection window: %s", nghttp2_strerror(rv));
    }

    connected_ = true;
    LOGI("Connected to %s:%u (HTTP/2 via nghttp2)", host.c_str(), port);
    return true;
}

// ============================================================
// disconnect()
// ============================================================

void GrpcClient::disconnect() {
    stop_streaming();
    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr;
    }
    if (sock_ >= 0) {
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
    }
    connected_ = false;
}

// ============================================================
// start_streaming()
// ============================================================

bool GrpcClient::start_streaming(const std::function<void(const uint8_t *, size_t)> &on_h264_frame) {
    if (streaming_) return true;
    if (!connected_) return false;

    on_frame_ = on_h264_frame;
    grpc_msg_buf_.reserve(512 * 1024);
    got_first_data_ = false;
    end_stream_retries_ = 0;
    consecutive_rst_on_idr_ = 0;
    ping_fail_count_.store(0);
    frame_count_ = 0;
    fps_start_ = std::chrono::steady_clock::now();

    if (!submit_onstart_request()) return false;
    if (!flush_nghttp2()) return false;

    streaming_ = true;
    stream_thread_ = std::thread(&GrpcClient::streaming_loop, this);
    LOGI("Streaming started on stream %d", static_cast<int>(active_stream_id_));
    return true;
}

// ============================================================
// stop_streaming()
// ============================================================

void GrpcClient::stop_streaming() {
    streaming_ = false;
    if (stream_thread_.joinable()) stream_thread_.join();
}

// ============================================================
// request_idr_frame() — thread-safe via atomic flag
// ============================================================

bool GrpcClient::request_idr_frame() {
    if (!connected_) return false;
    // Don't retry IDR if server consistently rejects with RST_STREAM
    if (consecutive_rst_on_idr_ >= 3) return false;
    idr_pending_.store(true);
    return true;
}

// ============================================================
// streaming_loop() — nghttp2 event loop
// ============================================================

void GrpcClient::streaming_loop() {
    LOGI("Streaming loop started (nghttp2), sock=%d", sock_);

    std::vector<uint8_t> recv_buf(256 * 1024);
    size_t recv_used = 0;

    int idle_count = 0;
    int ping_sent_count = 0;
    int stall_retries = 0;
    ping_ack_received_ = false;
    const int kPingIntervalSec = 8;
    const int kMaxPingWithoutResponse = 3;
    const int kMaxEndStreamRetries = 500;
    const int kMaxStallRetries = 10;
    const int kStallTimeoutSec = 10;
    const int kNoResponseTimeoutSec = 5;
    const int kTicksPerSec = 20;  // select timeout is 50ms

    while (streaming_ && connected_) {
        // FPS logging
        auto now = std::chrono::steady_clock::now();
        auto fps_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - fps_start_).count();
        if (fps_elapsed >= 2 && frame_count_ > 0) {
            LOGE("[FPS] %.1f (%d frames in %llds)",
                 frame_count_ / (double)fps_elapsed, frame_count_, (long long)fps_elapsed);
            frame_count_ = 0;
            fps_start_ = now;
        }

        // Check for pending IDR request from UI thread
        if (idr_pending_.exchange(false)) {
            if (submit_idr_request()) {
                flush_nghttp2();
            }
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(static_cast<socket_t>(sock_), &readfds);
        struct timeval tv = {0, 50000};
        int sel = select(static_cast<int>(sock_) + 1, &readfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            LOGI("select failed, exiting streaming loop");
            connected_ = false;
            break;
        }

        if (sel == 0) {
            idle_count++;

            // Check PING ACK at every idle tick (not just at PING send time)
            if (ping_ack_received_) {
                ping_ack_received_ = false;
                ping_sent_count = 0;
            }

            // Keepalive PING when idle
            if (got_first_data_ && idle_count > 0 &&
                (idle_count % (kPingIntervalSec * kTicksPerSec)) == 0) {
                if (ping_sent_count < kMaxPingWithoutResponse) {
                    uint8_t opaque[8] = {0x68, 0x73, 0x63, 0x72, 0x63, 0x70, 0x79, 0x00};
                    opaque[7] = static_cast<uint8_t>(ping_sent_count);
                    int rv = nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, opaque);
                    if (rv == 0) {
                        flush_nghttp2();
                        ping_sent_count++;
                        ping_fail_count_.store(ping_sent_count);
                        LOGI("Sent keepalive PING (idle=%ds, sent=%d)", idle_count / kTicksPerSec, ping_sent_count);
                    }
                } else {
                    LOGW("No PING ACK after %d attempts, connection dead", ping_sent_count);
                    connected_ = false;
                    break;
                }
            }

            // No response headers timeout
            if (!got_response_headers_ && idle_count >= kNoResponseTimeoutSec * kTicksPerSec) {
                LOGW("No response headers on stream %d after %ds (retries=%d/%d)",
                     static_cast<int>(active_stream_id_), kNoResponseTimeoutSec,
                     end_stream_retries_, kMaxEndStreamRetries);
                if (end_stream_retries_ < kMaxEndStreamRetries) {
                    end_stream_retries_++;
                    submit_onstart_request();
                    flush_nghttp2();
                    idle_count = 0;
                } else {
                    streaming_ = false;
                    break;
                }
            }

            // Stream stall detection: only reopen if we never got data on this stream.
            // If we got data then the server paused, that's normal (static screen).
            // The idle timeout (300s) will catch truly dead connections.
            if (!got_first_data_ && idle_count >= kStallTimeoutSec * kTicksPerSec &&
                ping_fail_count_.load() < kPingFailThreshold) {
                if (stall_retries < kMaxStallRetries) {
                    stall_retries++;
                    submit_onstart_request();
                    flush_nghttp2();
                    idle_count = 0;
                    LOGE("No data on stream, retrying (retry %d)", stall_retries);
                    continue;
                } else {
                    streaming_ = false;
                    break;
                }
            }

            // Idle timeout: PING alive but no DATA for too long
            if (got_first_data_ && idle_count >= idle_timeout_sec_ * kTicksPerSec &&
                ping_fail_count_.load() < kPingFailThreshold) {
                LOGI("No data for %ds but PING alive, server stopped producing frames", idle_timeout_sec_);
                connected_ = false;
                break;
            }
            continue;
        }

        // Data available — drain the socket
        idle_count = 0;
        ping_sent_count = 0;
        ping_fail_count_.store(0);

        bool draining = true;
        while (draining && streaming_ && connected_) {
            int n = ::recv(static_cast<socket_t>(sock_),
                           reinterpret_cast<char *>(recv_buf.data() + recv_used),
                           static_cast<int>(recv_buf.size() - recv_used), 0);
            if (n == 0) {
                LOGI("recv returned 0 (connection closed), exiting streaming loop");
                connected_ = false;
                break;
            }
            if (n < 0) {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    // No more data right now — normal end of drain
                    break;
                }
                LOGI("recv error %d, exiting streaming loop", err);
                connected_ = false;
                break;
            }
            recv_used += n;

            // Feed data to nghttp2
            nghttp2_ssize processed = nghttp2_session_mem_recv2(session_,
                recv_buf.data(), recv_used);
            if (processed < 0) {
                LOGE("nghttp2_session_mem_recv2 error: %s", nghttp2_strerror(static_cast<int>(processed)));
                connected_ = false;
                break;
            }

            size_t consumed = static_cast<size_t>(processed);
            if (consumed < recv_used) {
                memmove(recv_buf.data(), recv_buf.data() + consumed, recv_used - consumed);
            }
            recv_used -= consumed;

            // Flush pending nghttp2 output
            if (!flush_nghttp2()) {
                connected_ = false;
                break;
            }

            // Grow recv buffer if nearly full
            if (recv_buf.size() - recv_used < 64 * 1024) {
                size_t new_size = (std::min)(recv_buf.size() * 2, size_t(4 * 1024 * 1024));
                if (new_size > recv_buf.size()) recv_buf.resize(new_size);
            }

            // Check if more data is immediately available
            fd_set check_fds;
            FD_ZERO(&check_fds);
            FD_SET(static_cast<socket_t>(sock_), &check_fds);
            struct timeval tv_nowait = {0, 0};
            draining = (select(static_cast<int>(sock_) + 1, &check_fds, nullptr, nullptr, &tv_nowait) > 0);
        }

        if (!connected_) break;
    }

    streaming_ = false;
    LOGI("Streaming loop ended, got_data=%d, connected=%d, stall=%d, end_stream=%d",
         got_first_data_, (int)connected_.load(), stall_retries, end_stream_retries_);
}

} // namespace hscrcpy
