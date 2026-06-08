#include "grpc_client.h"
#include "log.h"
#include <cstring>
#include <cstdio>

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

static void write_frame(std::vector<uint8_t> &out, uint8_t type, uint8_t flags,
                        uint32_t stream_id, const uint8_t *payload, size_t len) {
    out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.push_back(type);
    out.push_back(flags);
    out.push_back(static_cast<uint8_t>((stream_id >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((stream_id >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((stream_id >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(stream_id & 0xFF));
    if (len > 0) out.insert(out.end(), payload, payload + len);
}

static void hpack_encode_literal_header_indexed_name(std::vector<uint8_t> &out,
                                                      uint8_t index,
                                                      const std::string &value) {
    out.push_back(index);
    uint32_t vlen = static_cast<uint32_t>(value.size());
    if (vlen < 0x7F) {
        out.push_back(static_cast<uint8_t>(vlen));
    } else {
        out.push_back(0x7F);
        uint32_t rem = vlen - 0x7F;
        while (rem >= 0x80) {
            out.push_back(static_cast<uint8_t>((rem & 0x7F) | 0x80));
            rem >>= 7;
        }
        out.push_back(static_cast<uint8_t>(rem));
    }
    out.insert(out.end(), value.begin(), value.end());
}

static void hpack_encode_literal_header_new_name(std::vector<uint8_t> &out,
                                                  const std::string &name,
                                                  const std::string &value) {
    out.push_back(0x00);
    uint32_t nlen = static_cast<uint32_t>(name.size());
    if (nlen < 0x7F) out.push_back(static_cast<uint8_t>(nlen));
    else { out.push_back(0x7F); uint32_t r = nlen - 0x7F; while (r >= 0x80) { out.push_back((r & 0x7F) | 0x80); r >>= 7; } out.push_back(r); }
    out.insert(out.end(), name.begin(), name.end());
    uint32_t vlen = static_cast<uint32_t>(value.size());
    if (vlen < 0x7F) out.push_back(static_cast<uint8_t>(vlen));
    else { out.push_back(0x7F); uint32_t r = vlen - 0x7F; while (r >= 0x80) { out.push_back((r & 0x7F) | 0x80); r >>= 7; } out.push_back(r); }
    out.insert(out.end(), value.begin(), value.end());
}

static void build_grpc_headers(std::vector<uint8_t> &hpack_block,
                                const std::string &path,
                                const std::string &authority) {
    hpack_block.push_back(0x83);
    hpack_block.push_back(0x86);
    hpack_encode_literal_header_indexed_name(hpack_block, 0x44, path);
    hpack_encode_literal_header_indexed_name(hpack_block, 0x41, authority);
    hpack_encode_literal_header_new_name(hpack_block, "content-type", "application/grpc");
    hpack_encode_literal_header_new_name(hpack_block, "te", "trailers");
    hpack_encode_literal_header_new_name(hpack_block, "user-agent", "grpc-java-netty/1.60.2");
    hpack_encode_literal_header_new_name(hpack_block, "grpc-accept-encoding", "gzip");
}

static void send_settings_ack(int sock) {
    uint8_t ack[9] = {0, 0, 0, 0x04, 0x01, 0, 0, 0, 0};
    send_all(sock, ack, 9);
}

static void send_window_update(int sock, uint32_t stream_id, uint32_t increment) {
    uint8_t payload[4];
    payload[0] = static_cast<uint8_t>((increment >> 24) & 0x7F);
    payload[1] = static_cast<uint8_t>((increment >> 16) & 0xFF);
    payload[2] = static_cast<uint8_t>((increment >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(increment & 0xFF);
    std::vector<uint8_t> frame;
    write_frame(frame, 0x08, 0x00, stream_id, payload, 4);
    send_all(sock, frame.data(), frame.size());
}

static void send_ping_ack(int sock, const uint8_t *opaque_data) {
    std::vector<uint8_t> frame;
    write_frame(frame, 0x06, 0x01, 0, opaque_data, 8);
    send_all(sock, frame.data(), frame.size());
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

struct Http2Settings {
    uint32_t header_table_size = 4096;
    bool enable_push = true;
    uint32_t max_concurrent_streams = 0xFFFFFFFF;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 0xFFFFFFFF;
};

static Http2Settings parse_settings(const uint8_t *payload, size_t len) {
    Http2Settings s;
    for (size_t i = 0; i + 5 <= len; i += 6) {
        uint16_t id = (static_cast<uint16_t>(payload[i]) << 8) | payload[i + 1];
        uint32_t val = (static_cast<uint32_t>(payload[i + 2]) << 24) |
                       (static_cast<uint32_t>(payload[i + 3]) << 16) |
                       (static_cast<uint32_t>(payload[i + 4]) << 8) |
                       static_cast<uint32_t>(payload[i + 5]);
        switch (id) {
        case 1: s.header_table_size = val; break;
        case 2: s.enable_push = val != 0; break;
        case 3: s.max_concurrent_streams = val; break;
        case 4: s.initial_window_size = val; break;
        case 5: s.max_frame_size = val; break;
        case 6: s.max_header_list_size = val; break;
        }
    }
    return s;
}

static std::vector<uint8_t> extract_h264_from_payload_map(const uint8_t *map_data, size_t map_len) {
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

        auto read_bytes = [&]() -> std::vector<uint8_t> {
            uint64_t len = read_varint();
            std::vector<uint8_t> result(map_data + pos, map_data + pos + len);
            pos += len;
            return result;
        };

        if (wtype == 2) {
            auto data = read_bytes();
            if (fnum == 1) {
                if (data.size() == 4 && data[0] == 'd' && data[1] == 'a' &&
                    data[2] == 't' && data[3] == 'a') {
                    // next field 2 is the ParamValue with val_bytes
                }
            } else if (fnum == 2) {
                size_t vp = 0;
                while (vp < data.size()) {
                    uint8_t vtag = data[vp++];
                    uint8_t vfnum = vtag >> 3;
                    uint8_t vwtype = vtag & 0x07;
                    if (vwtype == 0) {
                        uint64_t vval = 0; uint32_t shift = 0;
                        while (vp < data.size()) { uint8_t b = data[vp++]; vval |= (uint64_t)(b & 0x7F) << shift; shift += 7; if (!(b & 0x80)) break; }
                    } else if (vwtype == 2) {
                        uint64_t vlen = 0; uint32_t shift = 0;
                        while (vp < data.size()) { uint8_t b = data[vp++]; vlen |= (uint64_t)(b & 0x7F) << shift; shift += 7; if (!(b & 0x80)) break; }
                        if (vfnum == 5 && vp + vlen <= data.size()) {
                            return std::vector<uint8_t>(data.begin() + vp, data.begin() + vp + vlen);
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
    return {};
}

static bool extract_h264_from_reply(const uint8_t *pb, size_t pb_len, std::vector<uint8_t> &out_h264) {
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

        auto read_bytes = [&]() -> std::vector<uint8_t> {
            uint64_t len = read_varint();
            std::vector<uint8_t> result(pb + pos, pb + pos + len);
            pos += len;
            return result;
        };

        if (wire_type == 2) {
            auto data = read_bytes();
            if (field_num == 3) {
                auto h264 = extract_h264_from_payload_map(data.data(), data.size());
                if (!h264.empty()) {
                    out_h264 = std::move(h264);
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

bool GrpcClient::connect(const std::string &host, uint16_t port) {
    if (connected_) disconnect();

    connected_host_ = host;
    connected_port_ = port;

    LOGI("GrpcClient::connect(%s, %u) starting", host.c_str(), port);

    sock_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (sock_ == static_cast<int>(INVALID_SOCK)) {
        LOGE("socket creation failed");
        return false;
    }

    int flag = 1;
    setsockopt(static_cast<socket_t>(sock_), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char *>(&flag), sizeof(flag));

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

    const char *preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if (!send_all(sock_, reinterpret_cast<const uint8_t *>(preface), strlen(preface))) {
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }

    uint8_t client_settings_payload[] = {
        0x00, 0x01, 0x00, 0x00, 0x10, 0x00,
        0x00, 0x03, 0x7F, 0xFF, 0xFF, 0xFF,
        0x00, 0x04, 0x00, 0x40, 0x00, 0x00,
        0x00, 0x05, 0x00, 0x01, 0x00, 0x00,
    };
    std::vector<uint8_t> client_settings_frame;
    write_frame(client_settings_frame, 0x04, 0x00, 0, client_settings_payload, sizeof(client_settings_payload));
    if (!send_all(sock_, client_settings_frame.data(), client_settings_frame.size())) {
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }

    std::vector<uint8_t> preface_buf(4096);
    size_t buf_used = 0;
    bool got_server_settings = false;
    int attempts = 0;

    while (attempts < 5 && !got_server_settings) {
        int n = recv_with_timeout(sock_, preface_buf.data() + buf_used,
                                  preface_buf.size() - buf_used, 2);
        if (n <= 0) {
            attempts++;
            continue;
        }
        buf_used += n;

        size_t pos = 0;
        while (pos + 9 <= buf_used) {
            uint32_t frame_len = (static_cast<uint32_t>(preface_buf[pos]) << 16) |
                                 (static_cast<uint32_t>(preface_buf[pos + 1]) << 8) |
                                 static_cast<uint32_t>(preface_buf[pos + 2]);
            uint8_t frame_type = preface_buf[pos + 3];
            uint8_t flags = preface_buf[pos + 4];
            uint32_t stream_id = (static_cast<uint32_t>(preface_buf[pos + 5]) << 24) |
                                 (static_cast<uint32_t>(preface_buf[pos + 6]) << 16) |
                                 (static_cast<uint32_t>(preface_buf[pos + 7]) << 8) |
                                 static_cast<uint32_t>(preface_buf[pos + 8]);
            stream_id &= 0x7FFFFFFF;

            size_t total = 9 + frame_len;
            if (pos + total > buf_used) break;

            if (frame_type == 0x04 && !(flags & 0x01)) {
                auto s = parse_settings(preface_buf.data() + pos + 9, frame_len);
                server_window_size_ = s.initial_window_size;
                send_settings_ack(sock_);
                got_server_settings = true;
            } else if (frame_type == 0x06 && !(flags & 0x01)) {
                send_ping_ack(sock_, preface_buf.data() + pos + 9);
            }
            pos += total;
        }
        if (pos > 0 && pos < buf_used) memmove(preface_buf.data(), preface_buf.data() + pos, buf_used - pos);
        buf_used -= pos;
    }

    if (!got_server_settings) {
        LOGE("Never received server SETTINGS");
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }

    if (buf_used > 0) {
        leftover_buf_.assign(preface_buf.begin(), preface_buf.begin() + buf_used);
        LOGI("Saved %zu leftover bytes from connect phase", buf_used);
    }

    connected_ = true;
    LOGI("Connected to %s:%u (HTTP/2), server_window=%u", host.c_str(), port, server_window_size_);
    return true;
}

void GrpcClient::disconnect() {
    stop_streaming();
    if (sock_ >= 0) {
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
    }
    connected_ = false;
}

bool GrpcClient::start_streaming(const std::function<void(const std::vector<uint8_t> &)> &on_h264_frame) {
    if (streaming_) return true;
    if (!connected_) return false;

    on_frame_ = on_h264_frame;

    std::vector<uint8_t> hpack_block;
    std::string authority = connected_host_ + ":" + std::to_string(connected_port_);
    build_grpc_headers(hpack_block, "/ScrcpyService/onStart", authority);

    std::vector<uint8_t> headers_frame;
    write_frame(headers_frame, 0x01, 0x04, 1,
                hpack_block.data(), hpack_block.size());

    if (!send_all(sock_, headers_frame.data(), headers_frame.size())) {
        LOGE("Failed to send HEADERS frame");
        return false;
    }

    uint8_t grpc_empty[5] = {0, 0, 0, 0, 0};
    std::vector<uint8_t> data_frame;
    write_frame(data_frame, 0x00, 0x01, 1, grpc_empty, 5);

    if (!send_all(sock_, data_frame.data(), data_frame.size())) {
        LOGE("Failed to send DATA frame (empty protobuf)");
        return false;
    }

    LOGI("Sent gRPC onStart request (HEADERS + empty DATA)");

    send_window_update(sock_, 0, 4194304 - 65535);

    streaming_ = true;
    stream_thread_ = std::thread(&GrpcClient::streaming_loop, this);
    return true;
}

void GrpcClient::stop_streaming() {
    streaming_ = false;
    if (stream_thread_.joinable()) stream_thread_.join();
}

bool GrpcClient::request_idr_frame() {
    if (!connected_) return false;

    uint32_t sid = next_stream_id_;
    next_stream_id_ += 2;

    std::vector<uint8_t> hpack_block;
    std::string authority = connected_host_ + ":" + std::to_string(connected_port_);
    build_grpc_headers(hpack_block, "/ScrcpyService/onRequestIDRFrame", authority);

    std::vector<uint8_t> headers_frame;
    write_frame(headers_frame, 0x01, 0x04, sid,
                hpack_block.data(), hpack_block.size());

    if (!send_all(sock_, headers_frame.data(), headers_frame.size())) {
        return false;
    }

    uint8_t grpc_empty[5] = {0, 0, 0, 0, 0};
    std::vector<uint8_t> data_frame;
    write_frame(data_frame, 0x00, 0x01, sid, grpc_empty, 5);

    return send_all(sock_, data_frame.data(), data_frame.size());
}

void GrpcClient::streaming_loop() {
    LOGI("Streaming loop started, sock=%d, streaming=%d, connected=%d", sock_, (int)streaming_.load(), (int)connected_.load());

    if (recv_buffer_.size() < 4 * 1024 * 1024)
        recv_buffer_.resize(4 * 1024 * 1024);
    size_t buf_used = 0;

    if (!leftover_buf_.empty()) {
        size_t copy_len = std::min(leftover_buf_.size(), recv_buffer_.size());
        memcpy(recv_buffer_.data(), leftover_buf_.data(), copy_len);
        buf_used = copy_len;
        leftover_buf_.clear();
        LOGI("Preloaded %zu bytes from connect phase", copy_len);
    }

    bool got_first_data = false;
    int idle_count = 0;
    uint32_t bytes_received_on_stream = 0;
    uint32_t window_update_threshold = 32768;

    std::vector<uint8_t> grpc_msg_buf;
    grpc_msg_buf.reserve(512 * 1024);
    size_t grpc_consumed = 0;
    uint32_t grpc_expected_len = 0;
    bool grpc_header_parsed = false;

    while (streaming_ && connected_) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(static_cast<socket_t>(sock_), &readfds);
        struct timeval tv = {3, 0};
        int sel = select(static_cast<int>(sock_) + 1, &readfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            LOGI("select returned %d, wsa=%d, exiting streaming loop", sel, WSAGetLastError());
            break;
        }
        if (sel == 0) {
            idle_count++;
            if (idle_count >= 100) {
                LOGI("No data for 300s, stream dead");
                break;
            }
            continue;
        }
        idle_count = 0;

        int n = ::recv(static_cast<socket_t>(sock_),
                       reinterpret_cast<char *>(recv_buffer_.data() + buf_used),
                       static_cast<int>(recv_buffer_.size() - buf_used), 0);
        if (n <= 0) {
            int err = WSAGetLastError();
            LOGI("recv returned %d, wsa=%d, buf_used=%zu, exiting streaming loop", n, err, buf_used);
            break;
        }
        buf_used += n;

        size_t pos = 0;
        while (pos + 9 <= buf_used) {
            uint32_t frame_len = (static_cast<uint32_t>(recv_buffer_[pos]) << 16) |
                                 (static_cast<uint32_t>(recv_buffer_[pos + 1]) << 8) |
                                 static_cast<uint32_t>(recv_buffer_[pos + 2]);
            uint8_t frame_type = recv_buffer_[pos + 3];
            uint8_t flags = recv_buffer_[pos + 4];
            uint32_t stream_id = (static_cast<uint32_t>(recv_buffer_[pos + 5]) << 24) |
                                 (static_cast<uint32_t>(recv_buffer_[pos + 6]) << 16) |
                                 (static_cast<uint32_t>(recv_buffer_[pos + 7]) << 8) |
                                 static_cast<uint32_t>(recv_buffer_[pos + 8]);
            stream_id &= 0x7FFFFFFF;

            size_t total = 9 + frame_len;
            if (pos + total > buf_used) break;

            if (frame_type == 0x00) {
                const uint8_t *payload = recv_buffer_.data() + pos + 9;
                bytes_received_on_stream += frame_len;

                if (stream_id == 1) {
                    if (grpc_consumed > 0 && grpc_consumed < grpc_msg_buf.size()) {
                        size_t remaining = grpc_msg_buf.size() - grpc_consumed;
                        memmove(grpc_msg_buf.data(), grpc_msg_buf.data() + grpc_consumed, remaining);
                        grpc_msg_buf.resize(remaining);
                    } else if (grpc_consumed >= grpc_msg_buf.size()) {
                        grpc_msg_buf.clear();
                    }
                    grpc_consumed = 0;
                    grpc_msg_buf.insert(grpc_msg_buf.end(), payload, payload + frame_len);

                    while (true) {
                        size_t avail = grpc_msg_buf.size() - grpc_consumed;
                        if (!grpc_header_parsed && avail >= 5) {
                            const uint8_t *hdr = grpc_msg_buf.data() + grpc_consumed;
                            grpc_expected_len = (static_cast<uint32_t>(hdr[1]) << 24) |
                                                (static_cast<uint32_t>(hdr[2]) << 16) |
                                                (static_cast<uint32_t>(hdr[3]) << 8) |
                                                static_cast<uint32_t>(hdr[4]);
                            grpc_header_parsed = true;
                        }

                        if (!grpc_header_parsed) break;

                        size_t needed = 5 + grpc_expected_len;
                        if (avail < needed) break;

                        std::vector<uint8_t> h264_data;
                        if (extract_h264_from_reply(grpc_msg_buf.data() + grpc_consumed + 5, grpc_expected_len, h264_data)) {
                            if (!h264_data.empty() && on_frame_) {
                                on_frame_(h264_data);
                                got_first_data = true;
                            }
                        }

                        grpc_consumed += needed;
                        grpc_header_parsed = false;
                        grpc_expected_len = 0;
                    }
                }

                if (bytes_received_on_stream >= window_update_threshold) {
                    send_window_update(sock_, stream_id, bytes_received_on_stream);
                    send_window_update(sock_, 0, bytes_received_on_stream);
                    bytes_received_on_stream = 0;
                }

                if (flags & 0x01) {
                    if (stream_id == 1) {
                        LOGI("Server closed stream 1 (END_STREAM)");
                        streaming_ = false;
                    }
                }
            } else if (frame_type == 0x01) {
                if (stream_id == 1 && !idr_requested_) {
                    idr_requested_ = true;
                }
            } else if (frame_type == 0x04) {
                if (!(flags & 0x01)) {
                    auto s = parse_settings(recv_buffer_.data() + pos + 9, frame_len);
                    server_window_size_ = s.initial_window_size;
                    send_settings_ack(sock_);
                }
            } else if (frame_type == 0x06) {
                if (!(flags & 0x01)) {
                    send_ping_ack(sock_, recv_buffer_.data() + pos + 9);
                }
            } else if (frame_type == 0x07) {
                LOGW("Received GOAWAY");
                streaming_ = false;
            } else if (frame_type == 0x03) {
                LOGW("Received RST_STREAM on stream %u", stream_id);
                if (stream_id == 1) {
                    streaming_ = false;
                }
            } else if (frame_type == 0x08) {
            }

            pos += total;
        }

        if (pos > 0) {
            if (pos < buf_used) {
                memmove(recv_buffer_.data(), recv_buffer_.data() + pos, buf_used - pos);
            }
            buf_used -= pos;
        }
    }

    streaming_ = false;
    LOGI("Streaming loop ended, got_data=%d, connected=%d", got_first_data, (int)connected_.load());
}

} // namespace hscrcpy
