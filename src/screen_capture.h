#ifndef HSCRCPY_SCREEN_CAPTURE_H
#define HSCRCPY_SCREEN_CAPTURE_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include "hdc_wrapper.h"
#include "grpc_client.h"
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
#include <condition_variable>
#include <deque>

namespace hscrcpy {

enum class CaptureMode {
    kGrpcStream,
    kSnapshotFallback,
};

class ScreenCapture {
public:
    ScreenCapture(HdcWrapper &hdc, const CaptureConfig &config = CaptureConfig{});
    ~ScreenCapture();

    bool start();
    void stop();

    bool get_latest_frame(std::shared_ptr<DecodedImage> &img);

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    bool running() const { return running_; }
    bool grpc_connected() const { return running_ && grpc_client_.is_connected(); }
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
    bool setup_snapshot_fallback();
    bool start_uitest_with_extension(bool force_restart = false);
    bool wait_for_extension_ready();
    bool setup_port_forward();
    bool reconnect_grpc_stream();
    std::string get_local_so_md5(const std::string &filepath);
    std::string find_matching_so_with_md5(const std::string &device_md5);
    std::string find_matching_agent_so();
    std::string build_extension_cmd() const;
    void kill_uitest_extension();
    void grpc_stream_loop();
    void decode_loop();
    void snapshot_loop();
    bool capture_one_snapshot();

    HdcWrapper &hdc_;
    CaptureConfig config_;
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    CaptureMode mode_ = CaptureMode::kGrpcStream;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::thread decode_thread_;
    std::mutex mutex_;
    std::shared_ptr<DecodedImage> latest_frame_;
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<bool> got_stream_data_{false};
    HANDLE frame_event_ = nullptr;

    std::mutex h264_queue_mutex_;
    std::condition_variable h264_queue_cv_;
    std::deque<std::shared_ptr<std::vector<uint8_t>>> h264_queue_;
    size_t max_h264_queue_size_ = 10;

    GrpcClient grpc_client_;
    H264Decoder decoder_;
    JpegDecoder jpeg_decoder_;
    uint16_t local_video_port_ = 0;
    uint16_t local_control_port_ = 0;
    uint16_t local_base_port_ = 0;
    bool use_abstract_socket_ = false;
    uint16_t uitest_port_ = 5000;
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
