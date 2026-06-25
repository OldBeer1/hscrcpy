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

struct H264Ref { const uint8_t *data; size_t size; };

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
// gRPC message parser reset
// ============================================================

void GrpcClient::reset_grpc_parser() {
    grpc_consumed_ = 0;
    grpc_buf_size_ = 0;
    grpc_header_parsed_ = false;
    grpc_expected_len_ = 0;
}

// ============================================================
// Helper: build nghttp2_nv array
// ============================================================

static nghttp2_nv make_nv(const char *name, const char *value, bool no_copy = false) {
    nghttp2_nv nv;
    nv.name = reinterpret_cast<uint8_t *>(const_cast<char *>(name));
    nv.value = reinterpret_cast<uint8_t *>(const_cast<char *>(value));
    nv.namelen = strlen(name);
    nv.valuelen = strlen(value);
    nv.flags = no_copy ? NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE
                       : NGHTTP2_NV_FLAG_NONE;
    return nv;
}

// ============================================================
// nghttp2 callbacks
// ============================================================

nghttp2_ssize GrpcClient::send_callback(nghttp2_session *, const uint8_t *data,
                                          size_t length, int, void *user_data) {
    auto *self = static_cast<GrpcClient *>(user_data);
    self->send_buf_.insert(self->send_buf_.end(), data, data + length);
    return static_cast<nghttp2_ssize>(length);
}

int GrpcClient::on_begin_headers_callback(nghttp2_session *, const nghttp2_frame *frame,
                                            void *user_data) {
    auto *self = static_cast<GrpcClient *>(user_data);
    if (frame->hd.stream_id == static_cast<int32_t>(self->active_stream_id_)) {
        self->got_response_headers_ = true;
    }
    return 0;
}

int GrpcClient::on_header_callback(nghttp2_session *, const nghttp2_frame *frame,
                                     const uint8_t *name, size_t namelen,
                                     const uint8_t *value, size_t valuelen,
                                     uint8_t, void *user_data) {
    auto *self = static_cast<GrpcClient *>(user_data);
    if (frame->hd.stream_id == static_cast<int32_t>(self->active_stream_id_) &&
        frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
        if (namelen == 11 && memcmp(name, "grpc-status", 11) == 0) {
            LOGI("gRPC trailers: grpc-status=%.*s", (int)valuelen, value);
        }
    }
    return 0;
}

int GrpcClient::on_data_chunk_recv_callback(nghttp2_session *, uint8_t,
                                              int32_t stream_id,
                                              const uint8_t *data, size_t len,
                                              void *user_data) {
    auto *self = static_cast<GrpcClient *>(user_data);

    if (stream_id != static_cast<int32_t>(self->active_stream_id_)) {
        return 0;
    }

    // Compact unconsumed data
    if (self->grpc_consumed_ > 0) {
        if (self->grpc_consumed_ < self->grpc_buf_size_) {
            size_t remaining = self->grpc_buf_size_ - self->grpc_consumed_;
            memmove(self->grpc_msg_buf_.data(), self->grpc_msg_buf_.data() + self->grpc_consumed_, remaining);
            self->grpc_buf_size_ = remaining;
        } else {
            self->grpc_buf_size_ = 0;
        }
        self->grpc_consumed_ = 0;
    }

    // Append new data
    size_t needed_size = self->grpc_buf_size_ + len;
    if (needed_size > self->grpc_msg_buf_.size()) {
        self->grpc_msg_buf_.resize(needed_size);
    } else if (self->grpc_msg_buf_.size() > 512 * 1024 && needed_size < self->grpc_msg_buf_.size() / 2) {
        self->grpc_msg_buf_.resize(needed_size);
    }
    memcpy(self->grpc_msg_buf_.data() + self->grpc_buf_size_, data, len);
    self->grpc_buf_size_ += len;

    // Parse gRPC messages: 5-byte header (1 flag + 4 length) + payload
    while (true) {
        size_t avail = self->grpc_buf_size_ - self->grpc_consumed_;
        if (!self->grpc_header_parsed_ && avail >= 5) {
            const uint8_t *hdr = self->grpc_msg_buf_.data() + self->grpc_consumed_;
            self->grpc_expected_len_ = (static_cast<uint32_t>(hdr[1]) << 24) |
                                       (static_cast<uint32_t>(hdr[2]) << 16) |
                                       (static_cast<uint32_t>(hdr[3]) << 8) |
                                       static_cast<uint32_t>(hdr[4]);
            self->grpc_header_parsed_ = true;
        }
        if (!self->grpc_header_parsed_) break;

        size_t needed = 5 + self->grpc_expected_len_;
        if (avail < needed) break;

        H264Ref h264_ref;
        if (extract_h264_ref_from_reply(self->grpc_msg_buf_.data() + self->grpc_consumed_ + 5,
                                         self->grpc_expected_len_, h264_ref)) {
            if (h264_ref.size > 0 && self->on_frame_) {
                self->on_frame_(h264_ref.data, h264_ref.size);
                self->got_first_data_ = true;
                self->consecutive_rst_on_idr_ = 0;
                self->end_stream_retries_ = 0;
            }
        }

        self->grpc_consumed_ += needed;
        self->grpc_header_parsed_ = false;
        self->grpc_expected_len_ = 0;
    }

    return 0;
}

int GrpcClient::on_stream_close_callback(nghttp2_session *, int32_t stream_id,
                                           uint32_t error_code, void *user_data) {
    auto *self = static_cast<GrpcClient *>(user_data);

    if (stream_id == static_cast<int32_t>(self->active_stream_id_)) {
        LOGI("Stream %u closed (error_code=%u), retries=%d",
             self->active_stream_id_, error_code, self->end_stream_retries_);

        static constexpr int kMaxEndStreamRetries = 500;
        if (self->end_stream_retries_ < kMaxEndStreamRetries) {
            self->end_stream_retries_++;
            self->submit_onstart_request();
            LOGI("New stream %u opened, continuing streaming", self->active_stream_id_);
        } else {
            LOGW("Max END_STREAM retries reached, stopping streaming");
            self->streaming_ = false;
        }
    } else if (stream_id > 1 && (stream_id & 1)) {
        self->consecutive_rst_on_idr_++;
        if (self->consecutive_rst_on_idr_ <= 2) {
            LOGI("RST_STREAM on client stream %d, retrying IDR (%d/2)",
                 stream_id, self->consecutive_rst_on_idr_);
            self->idr_pending_.store(true);
        } else {
            LOGW("RST_STREAM on client stream %d, %d consecutive failures",
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
        // Either server's initial SETTINGS or SETTINGS ACK means handshake progress
        // We consider connection established once we've received any SETTINGS frame
        self->settings_ack_received_ = true;
        LOGI("HTTP/2 SETTINGS received (flags=0x%02x, stream_id=%d)", frame->hd.flags, frame->hd.stream_id);
        break;
    case NGHTTP2_PING:
        if (frame->hd.flags & NGHTTP2_FLAG_ACK) {
            self->ping_sent_count_ = 0;
            self->ping_fail_count_.store(0);
            LOGI("Received PING ACK, connection alive");
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
// ============================================================

bool GrpcClient::flush_nghttp2() {
    send_buf_.clear();
    const uint8_t *data_ptr = nullptr;
    while (true) {
        nghttp2_ssize n = nghttp2_session_mem_send2(session_, &data_ptr);
        if (n <= 0) break;
        send_buf_.insert(send_buf_.end(), data_ptr, data_ptr + n);
    }
    if (!send_buf_.empty()) {
        if (!send_all(sock_, send_buf_.data(), send_buf_.size())) {
            LOGE("Failed to send nghttp2 output (%zu bytes)", send_buf_.size());
            return false;
        }
    }
    return true;
}

// ============================================================
// Data source callbacks for gRPC empty payload (5 bytes)
// ============================================================

static nghttp2_ssize grpc_empty_data_read_callback(nghttp2_session *, int32_t,
                                                     uint8_t *buf, size_t length,
                                                     uint32_t *data_flags,
                                                     nghttp2_data_source *, void *) {
    static const uint8_t grpc_empty[5] = {0, 0, 0, 0, 0};
    size_t to_copy = (std::min)(length, static_cast<size_t>(5));
    memcpy(buf, grpc_empty, to_copy);
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<nghttp2_ssize>(to_copy);
}

// ============================================================
// Submit gRPC onStart request via nghttp2
// ============================================================

bool GrpcClient::submit_onstart_request() {
    std::string authority = connected_host_ + ":" + std::to_string(connected_port_);

    nghttp2_nv hdrs[] = {
        make_nv(":method", "POST"),
        make_nv(":path", "/ScrcpyService/onStart"),
        make_nv(":scheme", "http"),
        make_nv(":authority", authority.c_str()),
        make_nv("content-type", "application/grpc"),
        make_nv("te", "trailers"),
        make_nv("grpc-accept-encoding", "gzip"),
        make_nv("user-agent", "grpc-java-netty/1.60.2"),
    };

    nghttp2_data_provider2 dp;
    dp.read_callback = grpc_empty_data_read_callback;

    int32_t sid = nghttp2_submit_request2(session_, nullptr, hdrs,
                                            sizeof(hdrs) / sizeof(hdrs[0]),
                                            &dp, nullptr);
    if (sid < 0) {
        LOGE("nghttp2_submit_request2 failed: %s", nghttp2_strerror(sid));
        return false;
    }

    active_stream_id_ = static_cast<uint32_t>(sid);
    next_stream_id_ = active_stream_id_ + 2;
    reset_grpc_parser();
    got_response_headers_ = false;

    if (!flush_nghttp2()) return false;

    LOGI("Sent gRPC onStart request on stream %u", active_stream_id_);
    return true;
}

// ============================================================
// Submit gRPC onRequestIDRFrame via nghttp2
// ============================================================

bool GrpcClient::submit_idr_request() {
    std::string authority = connected_host_ + ":" + std::to_string(connected_port_);

    nghttp2_nv hdrs[] = {
        make_nv(":method", "POST"),
        make_nv(":path", "/ScrcpyService/onRequestIDRFrame"),
        make_nv(":scheme", "http"),
        make_nv(":authority", authority.c_str()),
        make_nv("content-type", "application/grpc"),
        make_nv("te", "trailers"),
        make_nv("grpc-accept-encoding", "gzip"),
        make_nv("user-agent", "grpc-java-netty/1.60.2"),
    };

    nghttp2_data_provider2 dp;
    dp.read_callback = grpc_empty_data_read_callback;

    int32_t sid = nghttp2_submit_request2(session_, nullptr, hdrs,
                                            sizeof(hdrs) / sizeof(hdrs[0]),
                                            &dp, nullptr);
    if (sid < 0) {
        LOGE("nghttp2_submit_request2 for IDR failed: %s", nghttp2_strerror(sid));
        return false;
    }

    next_stream_id_ = static_cast<uint32_t>(sid) + 2;

    if (!flush_nghttp2()) return false;

    LOGI("Sent gRPC onRequestIDRFrame on stream %d", sid);
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

    // Create nghttp2 session
    nghttp2_session_callbacks *cbs;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback2(cbs, send_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv_callback);

    int ret = nghttp2_session_client_new2(&session_, cbs, this, nullptr);
    nghttp2_session_callbacks_del(cbs);

    if (ret != 0) {
        LOGE("nghttp2_session_client_new2 failed: %s", nghttp2_strerror(ret));
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }

    // Submit client SETTINGS
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, nullptr, 0);

    // Send connection preface (24 bytes + SETTINGS frame)
    if (!flush_nghttp2()) {
        LOGE("Failed to send HTTP/2 connection preface");
        nghttp2_session_del(session_);
        session_ = nullptr;
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }

    // Wait for server SETTINGS + SETTINGS ACK
    settings_ack_received_ = false;
    recv_buf_.resize(64 * 1024);
    recv_used_ = 0;

    int attempts = 0;
    while (attempts < 10 && !settings_ack_received_) {
        int n = recv_with_timeout(sock_, recv_buf_.data() + recv_used_,
                                  recv_buf_.size() - recv_used_, 2);
        if (n <= 0) {
            attempts++;
            continue;
        }
        recv_used_ += n;

        nghttp2_ssize processed = nghttp2_session_mem_recv2(session_,
            recv_buf_.data(), recv_used_);
        if (processed < 0) {
            LOGE("nghttp2_session_mem_recv2 error: %s", nghttp2_strerror(static_cast<int>(processed)));
            nghttp2_session_del(session_);
            session_ = nullptr;
            CLOSE_SOCKET(static_cast<socket_t>(sock_));
            sock_ = -1;
            return false;
        }

        size_t consumed = static_cast<size_t>(processed);
        if (consumed < recv_used_) {
            memmove(recv_buf_.data(), recv_buf_.data() + consumed, recv_used_ - consumed);
        }
        recv_used_ -= consumed;

        // Flush any pending output (SETTINGS ACK, etc.)
        flush_nghttp2();
    }

    if (!settings_ack_received_) {
        LOGE("Never received server SETTINGS ACK");
        nghttp2_session_del(session_);
        session_ = nullptr;
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
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
    settings_ack_received_ = false;
    recv_used_ = 0;
    send_buf_.clear();
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
    stall_retries_ = 0;
    consecutive_rst_on_idr_ = 0;
    ping_sent_count_ = 0;
    ping_fail_count_.store(0);

    if (!submit_onstart_request()) {
        return false;
    }

    streaming_ = true;
    stream_thread_ = std::thread(&GrpcClient::streaming_loop, this);
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
// request_idr_frame() — thread-safe atomic flag
// ============================================================

bool GrpcClient::request_idr_frame() {
    if (!connected_) return false;
    idr_pending_.store(true);
    return true;
}

// ============================================================
// streaming_loop() — nghttp2 event loop
// ============================================================

void GrpcClient::streaming_loop() {
    LOGI("Streaming loop started (nghttp2), sock=%d", sock_);

    if (recv_buf_.size() < 256 * 1024)
        recv_buf_.resize(256 * 1024);
    recv_used_ = 0;

    int idle_count = 0;
    const int kPingIntervalSec = 15;
    const int kMaxPingWithoutResponse = 3;
    const int kMaxEndStreamRetries = 500;
    const int kMaxStallRetries = 10;
    const int kStallTimeoutSec = 30;
    const int kNoResponseTimeoutSec = 5;

    while (streaming_ && connected_) {
        // Check for pending IDR request (thread-safe)
        if (idr_pending_.exchange(false)) {
            if (!submit_idr_request()) {
                LOGW("Failed to submit IDR request");
            }
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(static_cast<socket_t>(sock_), &readfds);
        struct timeval tv = {0, 500000};
        int sel = select(static_cast<int>(sock_) + 1, &readfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            LOGI("select returned %d, wsa=%d, exiting streaming loop", sel, WSAGetLastError());
            connected_ = false;
            break;
        }

        if (sel == 0) {
            idle_count++;

            // Keepalive PING when idle
            if (got_first_data_ && idle_count > 0 &&
                (idle_count % (kPingIntervalSec * 2)) == 0) {
                uint8_t opaque[8] = {0x68, 0x73, 0x63, 0x72, 0x63, 0x70, 0x79, 0x00};
                opaque[7] = static_cast<uint8_t>(ping_sent_count_);
                int ret = nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, opaque);
                if (ret != 0) {
                    LOGW("nghttp2_submit_ping failed: %s", nghttp2_strerror(ret));
                    connected_ = false;
                    break;
                }
                if (!flush_nghttp2()) {
                    LOGW("Failed to send keepalive PING, connection is dead");
                    connected_ = false;
                    break;
                }
                ping_sent_count_++;
                ping_fail_count_.store(ping_sent_count_);
                LOGI("Sent keepalive PING (idle=%ds, ping_sent=%d)", idle_count / 2, ping_sent_count_);

                if (ping_sent_count_ >= kMaxPingWithoutResponse) {
                    LOGW("No response after %d keepalive PINGs, connection is dead", ping_sent_count_);
                    connected_ = false;
                    break;
                }
            }

            // No response headers timeout
            if (!got_response_headers_ && idle_count >= kNoResponseTimeoutSec * 2) {
                LOGW("No response headers on stream %u after %ds (retries=%d/%d)",
                     active_stream_id_, kNoResponseTimeoutSec, end_stream_retries_, kMaxEndStreamRetries);
                if (end_stream_retries_ < kMaxEndStreamRetries) {
                    end_stream_retries_++;
                    submit_onstart_request();
                    LOGI("New stream %u opened (no response retry %d)", active_stream_id_, end_stream_retries_);
                    idle_count = 0;
                } else {
                    LOGW("Max no-response retries reached, stopping streaming");
                    streaming_ = false;
                    break;
                }
            }

            // Stream stall detection
            if (got_first_data_ && idle_count >= kStallTimeoutSec * 2 &&
                ping_sent_count_ == 0) {
                LOGW("Stream stalled: no data for %ds (stall_retries=%d/%d)",
                     kStallTimeoutSec, stall_retries_, kMaxStallRetries);
                if (stall_retries_ < kMaxStallRetries) {
                    stall_retries_++;
                    submit_onstart_request();
                    got_first_data_ = false;
                    idle_count = 0;
                    LOGI("New stream %u opened after stall (retry %d)", active_stream_id_, stall_retries_);
                    continue;
                } else {
                    LOGW("Max stall retries reached, stopping streaming");
                    streaming_ = false;
                    break;
                }
            }

            // Idle timeout
            if (got_first_data_ && idle_count >= idle_timeout_sec_ * 2 && ping_sent_count_ == 0) {
                LOGI("No data for %ds but PING alive, server stopped producing frames", idle_timeout_sec_);
                connected_ = false;
                break;
            }
            continue;
        }

        // Data available
        idle_count = 0;
        ping_sent_count_ = 0;
        ping_fail_count_.store(0);

        int n = ::recv(static_cast<socket_t>(sock_),
                       reinterpret_cast<char *>(recv_buf_.data() + recv_used_),
                       static_cast<int>(recv_buf_.size() - recv_used_), 0);
        if (n <= 0) {
            int err = WSAGetLastError();
            LOGI("recv returned %d, wsa=%d, recv_used=%zu, exiting streaming loop", n, err, recv_used_);
            connected_ = false;
            break;
        }
        recv_used_ += n;

        // Feed data to nghttp2
        nghttp2_ssize processed = nghttp2_session_mem_recv2(session_,
            recv_buf_.data(), recv_used_);
        if (processed < 0) {
            LOGE("nghttp2_session_mem_recv2 error: %s", nghttp2_strerror(static_cast<int>(processed)));
            connected_ = false;
            break;
        }

        size_t consumed = static_cast<size_t>(processed);
        if (consumed < recv_used_) {
            memmove(recv_buf_.data(), recv_buf_.data() + consumed, recv_used_ - consumed);
        }
        recv_used_ -= consumed;

        // Flush any pending nghttp2 output (WINDOW_UPDATE, PING ACK, etc.)
        if (!flush_nghttp2()) {
            connected_ = false;
            break;
        }

        // Grow recv buffer if nearly full
        if (recv_buf_.size() - recv_used_ < 64 * 1024) {
            size_t new_size = recv_buf_.size() * 2;
            if (new_size > 4 * 1024 * 1024) new_size = 4 * 1024 * 1024;
            if (new_size > recv_buf_.size()) {
                recv_buf_.resize(new_size);
            }
        }
    }

    streaming_ = false;
    LOGI("Streaming loop ended, got_data=%d, connected=%d, stall_retries=%d, end_stream_retries=%d",
         got_first_data_, (int)connected_.load(), stall_retries_, end_stream_retries_);
}

} // namespace hscrcpy
