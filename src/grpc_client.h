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
#include <mutex>

namespace hscrcpy {

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

    // nghttp2 callbacks
    static nghttp2_ssize send_callback(nghttp2_session *session, const uint8_t *data,
                                        size_t length, int flags, void *user_data);
    static int on_begin_headers_callback(nghttp2_session *session,
                                          const nghttp2_frame *frame, void *user_data);
    static int on_header_callback(nghttp2_session *session,
                                   const nghttp2_frame *frame,
                                   const uint8_t *name, size_t namelen,
                                   const uint8_t *value, size_t valuelen,
                                   uint8_t flags, void *user_data);
    static int on_data_chunk_recv_callback(nghttp2_session *session,
                                            uint8_t flags,
                                            int32_t stream_id,
                                            const uint8_t *data, size_t len,
                                            void *user_data);
    static int on_stream_close_callback(nghttp2_session *session,
                                         int32_t stream_id,
                                         uint32_t error_code, void *user_data);
    static int on_frame_recv_callback(nghttp2_session *session,
                                       const nghttp2_frame *frame, void *user_data);

    int sock_ = -1;
    std::string connected_host_;
    uint16_t connected_port_ = 0;
    std::atomic<bool> connected_{false};
    std::atomic<bool> streaming_{false};
    std::thread stream_thread_;
    std::function<void(const uint8_t *, size_t)> on_frame_;
    std::mutex mutex_;

    // nghttp2 session
    nghttp2_session *session_ = nullptr;

    // Connection handshake
    bool settings_ack_received_ = false;

    // Stream management
    uint32_t active_stream_id_ = 1;
    uint32_t next_stream_id_ = 3;
    std::atomic<bool> idr_pending_{false};

    // gRPC message parser state (per active stream)
    std::vector<uint8_t> grpc_msg_buf_;
    size_t grpc_consumed_ = 0;
    size_t grpc_buf_size_ = 0;
    uint32_t grpc_expected_len_ = 0;
    bool grpc_header_parsed_ = false;

    // Network buffers
    std::vector<uint8_t> recv_buf_;
    size_t recv_used_ = 0;
    std::vector<uint8_t> send_buf_;

    // Streaming state
    bool got_first_data_ = false;
    bool got_response_headers_ = false;
    int end_stream_retries_ = 0;
    int stall_retries_ = 0;
    int consecutive_rst_on_idr_ = 0;

    // Keepalive / idle detection
    int idle_timeout_sec_ = 300;
    static constexpr int kPingFailThreshold = 2;
    std::atomic<int> ping_fail_count_{0};
    int ping_sent_count_ = 0;
};

} // namespace hscrcpy

#endif
