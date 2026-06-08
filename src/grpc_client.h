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

    bool start_streaming(const std::function<void(const std::vector<uint8_t> &)> &on_h264_frame);
    void stop_streaming();
    bool request_idr_frame();

private:
    bool send_grpc_request(const std::string &path, const std::vector<uint8_t> &payload);
    bool recv_grpc_response();
    bool parse_reply_message(const uint8_t *data, size_t size, GrpcReplyMessage &msg);
    void streaming_loop();

    int sock_ = -1;
    std::string connected_host_;
    uint16_t connected_port_ = 0;
    std::atomic<bool> connected_{false};
    std::atomic<bool> streaming_{false};
    std::thread stream_thread_;
    std::function<void(const std::vector<uint8_t> &)> on_frame_;
    std::mutex mutex_;
    std::vector<uint8_t> recv_buffer_;
    std::vector<uint8_t> leftover_buf_;
    uint32_t server_window_size_ = 65535;
    bool idr_requested_ = false;
    uint32_t next_stream_id_ = 3;
};

} // namespace hscrcpy

#endif
