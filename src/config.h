#ifndef HSCRCPY_CONFIG_H
#define HSCRCPY_CONFIG_H

#include <cstdint>
#include <string>

namespace hscrcpy {

struct AppConfig {
    int usb_scale = 1;
    int usb_bitrate = 31457280;
    int usb_frame_rate = 60;

    int tcp_scale = 1;
    int tcp_bitrate = 31457280;
    int tcp_frame_rate = 60;

    int local_base_port = 10000;
    int local_port_range = 500;

    int idle_timeout_sec = 300;
    int iframe_interval = 2000;
    int max_h264_queue = 10;
    int max_grpc_reconnect = 3;

    int scroll_pixels_per_notch = 200;
    int scroll_max = 800;

    static AppConfig &instance();
};

void config_load();
void config_save();

} // namespace hscrcpy

#endif
