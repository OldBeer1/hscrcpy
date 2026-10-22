#include "websocket_client.h"
#include "net_util.h"
#include "log.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <random>
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

// SHA1 + Base64 for the RFC6455 Sec-WebSocket-Accept handshake.
// Provided by FFmpeg's libavutil (already linked into hscrcpy).
// FFmpeg public headers lack extern "C" guards; wrap them like h264_decoder.cpp.
extern "C" {
#include <libavutil/base64.h>
#include <libavutil/mem.h>
}

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

WebSocketClient::WebSocketClient() : sock_(-1) { init_wsa(); }
WebSocketClient::~WebSocketClient() { disconnect(); }

// ============================================================
// Socket helpers (same pattern as GrpcClient)
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

// Read exactly len bytes (TCP may deliver partial reads; loop until complete
// or the connection dies / times out).
static bool recv_exact(int sock, uint8_t *buf, size_t len, int timeout_sec) {
    size_t got = 0;
    while (got < len) {
        int n = recv_with_timeout(sock, buf + got, len - got, timeout_sec);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

// ============================================================
// RFC6455 handshake helpers
// ============================================================

// Generate a random 16-byte client key, base64-encoded (RFC6455 §4.1).
static std::string ws_generate_key() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    uint8_t key[16];
    for (auto &b : key) b = static_cast<uint8_t>(dist(gen));
    char b64[64];
    av_base64_encode(b64, sizeof(b64), key, sizeof(key));
    return std::string(b64);
}

// ============================================================
// connect() — TCP + WebSocket handshake
// ============================================================

bool WebSocketClient::connect(const std::string &host, uint16_t port) {
    if (connected_) disconnect();

    connected_host_ = host;
    connected_port_ = port;
    LOGI("WebSocketClient::connect(%s, %u) starting", host.c_str(), port);

    sock_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (sock_ == static_cast<int>(INVALID_SOCK)) {
        LOGE("WebSocket socket creation failed");
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
#ifdef _WIN32
        int err = WSAGetLastError();
#else
        int err = errno;
#endif
        LOGE("WebSocket connect to %s:%u failed (err %d)", host.c_str(), port, err);
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }
    LOGI("WebSocket TCP connected to %s:%u", host.c_str(), port);

    // Build handshake request
    std::string key = ws_generate_key();
    char req[512];
    snprintf(req, sizeof(req),
             "GET / HTTP/1.1\r\n"
             "Host: %s:%u\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Protocol: scrcpy\r\n"
             "\r\n",
             host.c_str(), port, key.c_str());
    if (!send_all(sock_, reinterpret_cast<const uint8_t *>(req), strlen(req))) {
        LOGE("Failed to send WebSocket handshake");
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }
    LOGI("WebSocket handshake sent: %s", req);

    // Read response headers (up to 8KB)
    std::vector<uint8_t> resp(8192);
    size_t used = 0;
    bool got_headers = false;
    for (int i = 0; i < 20 && !got_headers; i++) {
        if (used >= resp.size()) break;
        int n = recv_with_timeout(sock_, resp.data() + used, resp.size() - used, 2);
        if (n <= 0) {
            LOGW("WebSocket handshake recv timeout/error (attempt %d)", i + 1);
            CLOSE_SOCKET(static_cast<socket_t>(sock_));
            sock_ = -1;
            return false;
        }
        used += n;
        // Look for end of headers: \r\n\r\n
        for (size_t p = 0; p + 3 < used; p++) {
            if (resp[p] == '\r' && resp[p + 1] == '\n' &&
                resp[p + 2] == '\r' && resp[p + 3] == '\n') {
                got_headers = true;
                break;
            }
        }
    }
    if (!got_headers) {
        LOGE("WebSocket handshake: no HTTP response headers received");
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }

    std::string resp_str(reinterpret_cast<char *>(resp.data()), used);
    LOGI("WebSocket handshake response: %s", resp_str.c_str());

    // Validate: HTTP 101 + Upgrade: websocket + Sec-WebSocket-Accept
    // Server may return "Upgrade: WebSocket" (case-insensitive per RFC6455).
    if (resp_str.find(" 101 ") == std::string::npos && resp_str.find("\r\n101 ") == std::string::npos) {
        LOGE("WebSocket handshake rejected (status not 101): %s", resp_str.c_str());
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
        return false;
    }
    {
        // Case-insensitive Upgrade header check
        std::string lower = resp_str;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("upgrade: websocket") == std::string::npos) {
            LOGE("WebSocket handshake missing Upgrade: websocket");
            CLOSE_SOCKET(static_cast<socket_t>(sock_));
            sock_ = -1;
            return false;
        }
    }

    connected_ = true;
    LOGI("WebSocket connected to %s:%u", host.c_str(), port);
    return true;
}

// ============================================================
// disconnect()
// ============================================================

void WebSocketClient::disconnect() {
    stop_streaming();
    if (sock_ >= 0) {
        CLOSE_SOCKET(static_cast<socket_t>(sock_));
        sock_ = -1;
    }
    connected_ = false;
}

// ============================================================
// Frame send (client → server, MUST be masked per RFC6455)
// ============================================================

bool WebSocketClient::send_frame(uint8_t opcode, const uint8_t *payload, size_t len) {
    if (!connected_ || sock_ < 0) return false;

    std::vector<uint8_t> hdr;
    hdr.push_back(static_cast<uint8_t>(0x80 | opcode));  // FIN + opcode

    size_t payload_len = len;
    // Client frames must be masked. Use a fixed 4-byte mask (per RFC the
    // mask is arbitrary as long as it's 32 bits; deterministic is fine).
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

    if (payload_len < 126) {
        hdr.push_back(static_cast<uint8_t>(0x80 | payload_len));
    } else if (payload_len <= 0xFFFF) {
        hdr.push_back(static_cast<uint8_t>(0x80 | 126));
        hdr.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
        hdr.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    } else {
        hdr.push_back(static_cast<uint8_t>(0x80 | 127));
        uint64_t l = static_cast<uint64_t>(payload_len);
        for (int i = 7; i >= 0; i--) hdr.push_back(static_cast<uint8_t>((l >> (8 * i)) & 0xFF));
    }
    hdr.insert(hdr.end(), mask, mask + 4);

    if (!send_all(sock_, hdr.data(), hdr.size())) return false;

    // Mask payload
    std::vector<uint8_t> masked(payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        masked[i] = payload[i] ^ mask[i % 4];
    }
    if (!send_all(sock_, masked.data(), masked.size())) return false;
    return true;
}

bool WebSocketClient::send_text(const std::string &msg) {
    LOGI("WS send text: %s", msg.c_str());
    bool ok = send_frame(0x1, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());
    if (!ok) LOGW("WS failed to send: %s", msg.c_str());
    return ok;
}

// ============================================================
// Frame read (server → client, NOT masked)
// ============================================================

bool WebSocketClient::read_frame(uint8_t &opcode, std::vector<uint8_t> &payload) {
    uint8_t hdr[2];
    if (!recv_exact(sock_, hdr, 2, 2)) return false;

    bool fin = (hdr[0] & 0x80) != 0;
    opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;

    if (len == 126) {
        uint8_t ext[2];
        if (!recv_exact(sock_, ext, 2, 2)) return false;
        len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (!recv_exact(sock_, ext, 8, 2)) return false;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }

    uint8_t mask_key[4] = {0, 0, 0, 0};
    if (masked) {
        if (!recv_exact(sock_, mask_key, 4, 2)) return false;
    }

    if (len > 64 * 1024 * 1024) {
        LOGE("WS frame too large (%llu bytes), aborting", (unsigned long long)len);
        return false;
    }

    payload.resize(static_cast<size_t>(len));
    if (len > 0 && !recv_exact(sock_, payload.data(), static_cast<size_t>(len), 2)) {
        return false;
    }
    if (masked) {
        for (size_t i = 0; i < payload.size(); i++) payload[i] ^= mask_key[i % 4];
    }
    if (!fin) {
        // Fragmented frames not expected from this server; treat as fatal.
        LOGE("WS fragmented frame (fin=0), unsupported");
        return false;
    }
    return true;
}

// ============================================================
// Text command → binary H264 frame stream parser
// ============================================================

void WebSocketClient::reset_parser() {
    bin_buf_size_ = 0;
}

// ============================================================
// start_streaming()
// ============================================================

bool WebSocketClient::start_streaming(const std::function<void(const uint8_t *, size_t)> &on_h264_frame) {
    if (streaming_) return true;
    if (!connected_) return false;

    on_frame_ = on_h264_frame;
    reset_parser();
    got_started_ = false;

    if (!send_text("{\"type\":\"start\"}")) return false;

    // Wait for {"type":"started"} (up to 10s) — server may need time to boot encoder.
    // Binary frames may arrive before 'started' (server pushes stream immediately);
    // buffer them into bin_buf_ so no H264 data is lost while we wait.
    std::vector<uint8_t> payload;
    uint8_t opcode = 0;
    auto start = std::chrono::steady_clock::now();
    while (!got_started_) {
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count() >= 10) {
            LOGE("WS start timeout: no 'started' response within 10s");
            return false;
        }
        if (!read_frame(opcode, payload)) {
            LOGE("WS error while waiting for 'started' response");
            return false;
        }
        if (opcode == 0x1) {  // text
            std::string text(reinterpret_cast<char *>(payload.data()), payload.size());
            LOGI("WS received text: %s", text.c_str());
            if (text.find("\"started\"") != std::string::npos) {
                got_started_ = true;
            } else if (text.find("\"stopped\"") != std::string::npos) {
                LOGW("WS received 'stopped' during start");
                return false;
            } else if (text.find("\"idrResponse\"") != std::string::npos) {
                LOGI("WS received 'idrResponse'");
            }
        } else if (opcode == 0x2) {  // binary — buffer until receiver loop starts
            bin_buf_.insert(bin_buf_.end(), payload.begin(), payload.end());
            bin_buf_size_ = bin_buf_.size();
        } else if (opcode == 0x8) {  // close
            LOGE("WS server sent close frame during start");
            return false;
        }
        // opcode 0x9/0xA (ping/pong): pings are handled in receiver_loop
    }

    LOGI("WS stream started (received 'started')");
    streaming_ = true;
    receiver_thread_ = std::thread(&WebSocketClient::receiver_loop, this);
    return true;
}

// ============================================================
// stop_streaming()
// ============================================================

void WebSocketClient::stop_streaming() {
    if (streaming_) {
        streaming_ = false;
        send_text("{\"type\":\"stop\"}");
    }
    if (receiver_thread_.joinable()) receiver_thread_.join();
}

// ============================================================
// request_idr_frame()
// ============================================================

bool WebSocketClient::request_idr_frame() {
    if (!connected_) return false;
    return send_text("{\"type\":\"requestIDR\"}");
}

// ============================================================
// receiver_loop() — read frames, dispatch text/binary
// ============================================================

void WebSocketClient::receiver_loop() {
    LOGI("WS receiver loop started, sock=%d", sock_);

    std::vector<uint8_t> payload;
    uint8_t opcode = 0;

    while (streaming_ && connected_) {
        if (!read_frame(opcode, payload)) {
            if (streaming_ && connected_) {
                LOGW("WS read_frame failed (timeout/error), stream ended");
            }
            break;
        }

        switch (opcode) {
        case 0x1: {  // text
            std::string text(reinterpret_cast<char *>(payload.data()), payload.size());
            LOGI("WS received text: %s", text.c_str());
            if (text.find("\"stopped\"") != std::string::npos) {
                LOGI("WS received 'stopped', ending stream");
                streaming_ = false;
            }
            break;
        }
        case 0x2: {  // binary: 4-byte big-endian length + H264 payload
            // HOScrcpy sends each binary WebSocket message as a complete
            // 4-byte-prefixed H264 chunk; concatenate into the frame parser.
            bin_buf_.insert(bin_buf_.end(), payload.begin(), payload.end());
            bin_buf_size_ = bin_buf_.size();

            size_t pos = 0;
            while (bin_buf_size_ - pos >= 4) {
                uint32_t frame_len =
                    (static_cast<uint32_t>(bin_buf_[pos]) << 24) |
                    (static_cast<uint32_t>(bin_buf_[pos + 1]) << 16) |
                    (static_cast<uint32_t>(bin_buf_[pos + 2]) << 8) |
                    static_cast<uint32_t>(bin_buf_[pos + 3]);
                if (frame_len == 0) {  // padding/keepalive zero-length marker
                    pos += 4;
                    continue;
                }
                if (bin_buf_size_ - pos - 4 < frame_len) break;  // incomplete, wait

                if (on_frame_) {
                    on_frame_(bin_buf_.data() + pos + 4, frame_len);
                }
                pos += 4 + frame_len;
            }

            // Compact consumed bytes
            if (pos > 0) {
                size_t remaining = bin_buf_size_ - pos;
                if (remaining > 0) {
                    memmove(bin_buf_.data(), bin_buf_.data() + pos, remaining);
                }
                bin_buf_.resize(remaining);
                bin_buf_size_ = remaining;
            }
            break;
        }
        case 0x8:  // close
            LOGI("WS received close frame, ending stream");
            streaming_ = false;
            break;
        case 0x9:  // ping → pong
            send_frame(0xA, payload.data(), payload.size());
            break;
        case 0xA:  // pong
            break;
        default:
            LOGW("WS unknown opcode %u", opcode);
            break;
        }
    }

    LOGI("WS receiver loop ended");
    connected_ = false;
}

} // namespace hscrcpy
