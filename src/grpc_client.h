#ifndef HSCRCPY_GRPC_CLIENT_H
#define HSCRCPY_GRPC_CLIENT_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <nghttp2/nghttp2.h>
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>

namespace hscrcpy {

struct H264Ref { const uint8_t *data; size_t size; };

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
    void streaming_loop();
    bool submit_onstart_request();
    bool submit_idr_request();
    bool flush_nghttp2();
    void reset_grpc_parser();

    // nghttp2 callbacks — only nghttp2_submit_* calls, no flush/send
    static int on_begin_headers_callback(nghttp2_session *, const nghttp2_frame *, void *);
    static int on_header_callback(nghttp2_session *, const nghttp2_frame *,
                                   const uint8_t *, size_t, const uint8_t *, size_t,
                                   uint8_t, void *);
    static int on_data_chunk_recv_callback(nghttp2_session *, uint8_t,
                                            int32_t, const uint8_t *, size_t, void *);
    static int on_stream_close_callback(nghttp2_session *, int32_t, uint32_t, void *);
    static int on_frame_recv_callback(nghttp2_session *, const nghttp2_frame *, void *);

    int sock_ = -1;
    std::string connected_host_;
    uint16_t connected_port_ = 0;
    std::atomic<bool> connected_{false};
    std::atomic<bool> streaming_{false};
    std::thread stream_thread_;
    std::function<void(const uint8_t *, size_t)> on_frame_;

    // nghttp2 session (accessed only from streaming thread + connect/disconnect)
    nghttp2_session *session_ = nullptr;

    // Connection handshake
    bool settings_received_ = false;

    // Stream tracking
    int32_t active_stream_id_ = -1;
    int32_t last_idr_stream_id_ = -1;
    std::atomic<bool> idr_pending_{false};

    // gRPC parser state (active stream only)
    std::vector<uint8_t> grpc_msg_buf_;
    size_t grpc_buf_size_ = 0;    // logical size (may differ from vector.size())
    bool got_response_headers_ = false;

    // Streaming state
    bool got_first_data_ = false;
    int end_stream_retries_ = 0;
    int consecutive_rst_on_idr_ = 0;

    // Keepalive / idle detection
    int idle_timeout_sec_ = 300;
    static constexpr int kPingFailThreshold = 2;
    std::atomic<int> ping_fail_count_{0};
    bool ping_ack_received_ = false;  // Set by PING ACK callback, checked by streaming loop

    // FPS counter (debug)
    int frame_count_ = 0;
    std::chrono::steady_clock::time_point fps_start_;
};

} // namespace hscrcpy

#endif
