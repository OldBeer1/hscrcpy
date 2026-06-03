#ifndef HSCRCPY_SCREEN_CAPTURE_H
#define HSCRCPY_SCREEN_CAPTURE_H

#include "hdc_wrapper.h"
#include "grpc_client.h"
#include "h264_decoder.h"
#include "jpeg_decoder.h"
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>

namespace hscrcpy {

enum class CaptureMode {
    kGrpcStream,
    kSnapshotFallback,
};

class ScreenCapture {
public:
    ScreenCapture(HdcWrapper &hdc, uint32_t fps = 30);
    ~ScreenCapture();

    bool start();
    void stop();

    bool get_latest_frame(std::shared_ptr<DecodedImage> &img);

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    bool running() const { return running_; }
    CaptureMode mode() const { return mode_; }
    uint64_t frame_count() const { return frame_count_; }
    const std::string &uitest_version() const { return uitest_version_; }
    uint16_t local_base_port() const { return local_base_port_; }
    HANDLE frame_event() const { return frame_event_; }

private:
    bool setup_grpc_stream();
    bool setup_snapshot_fallback();
    bool push_server_so();
    bool start_uitest_with_extension();
    bool setup_port_forward();
    std::string get_uitest_version();
    std::string get_device_so_md5();
    std::string get_local_so_md5(const std::string &filepath);
    std::string find_matching_so_with_md5(const std::string &device_md5);
    std::string find_matching_agent_so();
    bool push_agent_so();
    void grpc_stream_loop();
    void snapshot_loop();
    bool capture_one_snapshot();

    HdcWrapper &hdc_;
    uint32_t fps_ = 30;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    CaptureMode mode_ = CaptureMode::kGrpcStream;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::shared_ptr<DecodedImage> latest_frame_;
    std::atomic<uint64_t> frame_count_{0};
    HANDLE frame_event_ = nullptr;

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
};

} // namespace hscrcpy

#endif
