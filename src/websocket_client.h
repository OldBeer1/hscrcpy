#ifndef HSCRCPY_WEBSOCKET_CLIENT_H
#define HSCRCPY_WEBSOCKET_CLIENT_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>

namespace hscrcpy {

// Minimal RFC6455 WebSocket client used for the 7.0 server SO streaming channel.
//
// Protocol (verified against HOScrcpy 2.13.0 ScrcpyWebSocketClient):
//   - ws://host:port  (no subprotocol, Draft_6455)
//   - text command frames: {"type":"start"} / {"type":"requestIDR"} / {"type":"stop"}
//   - server replies text: {"type":"started"} / {"type":"stopped"} / {"type":"idrResponse"}
//   - binary frames: 4-byte big-endian length prefix + H264 payload
class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    // TCP connect + WebSocket handshake (blocking, with timeout)
    bool connect(const std::string &host, uint16_t port);
    void disconnect();

    bool is_connected() const { return connected_; }
    bool is_streaming_active() const { return streaming_; }

    // Opens the stream: sends {"type":"start"} and waits up to 10s for
    // {"type":"started"}. Once started, on_h264_frame is called from the
    // internal receiver thread for each decoded 4-byte-prefixed H264 frame.
    bool start_streaming(const std::function<void(const uint8_t *, size_t)> &on_h264_frame);
    void stop_streaming();

    // Sends {"type":"requestIDR"} (thread-safe).
    bool request_idr_frame();

private:
    void receiver_loop();
    bool send_text(const std::string &msg);
    bool send_frame(uint8_t opcode, const uint8_t *payload, size_t len);
    // Reads one WebSocket frame header + payload from sock_.
    // Returns true on a complete frame; fills opcode/payload.
    bool read_frame(uint8_t &opcode, std::vector<uint8_t> &payload);
    void reset_parser();

    int sock_ = -1;
    std::string connected_host_;
    uint16_t connected_port_ = 0;
    std::atomic<bool> connected_{false};
    std::atomic<bool> streaming_{false};
    std::thread receiver_thread_;
    std::function<void(const uint8_t *, size_t)> on_frame_;

    // Parsing state for binary frame stream (4-byte big-endian length + payload)
    std::vector<uint8_t> bin_buf_;
    size_t bin_buf_size_ = 0;

    // Started handshake latch
    bool got_started_ = false;
};

} // namespace hscrcpy

#endif // HSCRCPY_WEBSOCKET_CLIENT_H
