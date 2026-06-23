#ifndef HSCRCPY_GRPC_CLIENT_H
#define HSCRCPY_GRPC_CLIENT_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

namespace hscrcpy {

struct GrpcReplyMessage {
    std::string data;
    int32_t reply_type = 0;
    std::vector<uint8_t> raw_payload;
};

class GrpcClient {
public:
    GrpcClient();
    ~GrpcClient();

    bool connect(const std::string &host, uint16_t port);
    void disconnect();
    bool is_connected() const { return connected_; }
    bool is_streaming_active() const { return streaming_; }
    bool is_ping_failing() const { return ping_fail_count_ >= kPingFailThreshold; }

    bool start_streaming(const std::function<void(const uint8_t *, size_t)> &on_h264_frame);
    void stop_streaming();
    bool request_idr_frame();

    void set_idle_timeout_sec(int sec) { idle_timeout_sec_ = sec; }

private:
    bool send_grpc_request(const std::string &path, const std::vector<uint8_t> &payload);
    bool recv_grpc_response();
    bool parse_reply_message(const uint8_t *data, size_t size, GrpcReplyMessage &msg);
    void streaming_loop();
    bool send_onstart_request(uint32_t stream_id);

    int sock_ = -1;
    std::string connected_host_;
    uint16_t connected_port_ = 0;
    std::atomic<bool> connected_{false};
    std::atomic<bool> streaming_{false};
    std::thread stream_thread_;
    std::function<void(const uint8_t *, size_t)> on_frame_;
    std::mutex mutex_;
    std::vector<uint8_t> recv_buffer_;
    std::vector<uint8_t> leftover_buf_;
    uint32_t server_window_size_ = 65535;
    bool idr_requested_ = false;
    uint32_t next_stream_id_ = 3;
    uint32_t active_stream_id_ = 1;
    int idle_timeout_sec_ = 300;
    static constexpr int kPingFailThreshold = 2;  // Early warning: 2 unanswered PINGs
    std::atomic<int> ping_fail_count_{0};
};

} // namespace hscrcpy

#endif
