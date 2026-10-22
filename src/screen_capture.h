#ifndef HSCRCPY_SCREEN_CAPTURE_H
#define HSCRCPY_SCREEN_CAPTURE_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include "hdc_wrapper.h"
#include "grpc_client.h"
#include "websocket_client.h"
#include "h264_decoder.h"
#include "jpeg_decoder.h"
#include "protocol.h"
#include "config.h"
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>

namespace hscrcpy {

enum class CaptureMode {
    kGrpcStream,        // SO < 7.0: HTTP/2 gRPC channel (abstract socket)
    kWebSocketStream,   // SO >= 7.0: RFC6455 WebSocket channel (TCP 5000)
    kSnapshotFallback,
};

// Custom window messages for UI notification (WM_USER + offset)
enum WmNotify : UINT {
    kMsgStartFailed       = WM_USER + 1,  // Screen capture start failed
    kMsgNoFrame           = WM_USER + 2,  // No frame received
    kMsgCaptureReady      = WM_USER + 3,  // Capture ready, resize canvas
    kMsgStopComplete      = WM_USER + 4,  // Stop mirroring complete
    kMsgLayoutUpdate      = WM_USER + 5,  // Layout window update
    kMsgConnectingStatus  = WM_USER + 6,  // Show "Connecting..." status
    kMsgConnLostStartup   = WM_USER + 7,  // Connection lost during startup
    kMsgDeviceDisconnected = WM_USER + 8,  // Device disconnected (hdc unreachable)
    kMsgStreamEnded       = WM_USER + 9,  // Stream loop ended
    kMsgFrameRender       = WM_USER + 10, // Event-driven frame rendering
    kMsgReconnecting      = WM_USER + 14, // Reconnecting stream (attempt N/M)
};

class ScreenCapture {
public:
    ScreenCapture(HdcWrapper &hdc, const CaptureConfig &config = CaptureConfig{});
    ~ScreenCapture();

    bool start();
    void stop();
    void request_stop() { running_ = false; hdc_.set_cancel(true); }  // Non-blocking: signal threads to exit

    bool get_latest_frame(std::shared_ptr<DecodedImage> &img);

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    bool running() const { return running_; }
    bool grpc_connected() const {
        if (!running_) return false;
        return use_webSocket_ ? webSocket_client_.is_connected() : grpc_client_.is_connected();
    }
    bool is_reconnecting() const { return reconnecting_; }
    CaptureMode mode() const { return mode_; }
    uint64_t frame_count() const { return frame_count_; }
    const std::string &uitest_version() const { return uitest_version_; }
    uint16_t local_base_port() const { return local_base_port_; }
    HANDLE frame_event() const { return frame_event_; }
    const CaptureConfig &config() const { return config_; }
    void set_notify_hwnd(HWND h) { notify_hwnd_ = h; }

private:
    bool setup_grpc_stream(bool force_restart = false);
    bool setup_webSocket_stream(bool force_restart = false);
    bool setup_snapshot_fallback();
    bool start_uitest_with_extension(bool force_restart = false);
    void query_device_facts();  // uitest version + device SO md5 (cached, once per connection)
    bool wait_for_extension_ready();
    bool setup_port_forward();
    void cleanup_forward_ports();
    bool reconnect_grpc_stream();
    bool reconnect_webSocket_stream();
    void grpc_stream_loop();
    void webSocket_stream_loop();
    std::string get_local_so_md5(const std::string &filepath);
    std::string find_matching_so_with_md5(const std::string &device_md5);
    std::vector<std::string> build_so_candidates(const std::string &device_md5);
    bool is_cloud_device();
    std::string find_matching_agent_so();
    std::string build_extension_cmd() const;
    void kill_uitest_extension();
    void snapshot_loop();
    bool capture_one_snapshot();

    HdcWrapper &hdc_;
    CaptureConfig config_;
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    CaptureMode mode_ = CaptureMode::kGrpcStream;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::shared_ptr<DecodedImage> latest_frame_;
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<bool> got_stream_data_{false};
    HANDLE frame_event_ = nullptr;

    GrpcClient grpc_client_;
    WebSocketClient webSocket_client_;
    H264Decoder decoder_;
    JpegDecoder jpeg_decoder_;
    uint16_t local_video_port_ = 0;
    uint16_t local_control_port_ = 0;
    uint16_t local_base_port_ = 0;
    bool use_abstract_socket_ = false;
    bool use_webSocket_ = false;  // SO >= 7.0 → WebSocket channel
    bool use_emulator_so_ = false;  // emulator SO → TCP 5000 gRPC (cloud main screen)
    uint16_t uitest_port_ = 5000;
    std::string forced_so_path_;  // Non-empty: force this SO (emulator fallback)
    // Cached device facts — queried once per connection (not per capture tier)
    bool version_queried_ = false;      // uitest_version_ already fetched this connection
    std::string cached_device_md5_;     // device SO md5, cached to avoid re-query per tier
    std::string uitest_version_;
    std::string server_dir_;
    int max_grpc_reconnect_ = 3;
    HWND notify_hwnd_ = nullptr;
    std::atomic<bool> reconnecting_{false};

    // Pre-allocated frame pool (3-buffer) to avoid per-frame make_shared
    std::array<std::shared_ptr<DecodedImage>, 3> frame_pool_;
    int pool_idx_ = 0;
};

} // namespace hscrcpy

#endif
