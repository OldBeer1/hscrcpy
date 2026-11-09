#include "config.h"
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace hscrcpy {

static AppConfig g_config;

AppConfig &AppConfig::instance() {
    return g_config;
}

static std::string ini_path() {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = '\0';
    return std::string(exe) + "\\hscrcpy.ini";
}

static int read_int(LPCSTR section, LPCSTR key, int default_val, const char *path) {
    return GetPrivateProfileIntA(section, key, default_val, path);
}

static void write_int(LPCSTR section, LPCSTR key, int val, const char *path) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    WritePrivateProfileStringA(section, key, buf, path);
}

void config_load() {
    std::string path = ini_path();
    AppConfig &c = g_config;

    c.usb_scale = read_int("Stream", "UsbScale", c.usb_scale, path.c_str());
    c.usb_bitrate = read_int("Stream", "UsbBitrate", c.usb_bitrate, path.c_str());
    c.usb_frame_rate = read_int("Stream", "UsbFrameRate", c.usb_frame_rate, path.c_str());

    c.tcp_scale = read_int("Stream", "TcpScale", c.tcp_scale, path.c_str());
    c.tcp_bitrate = read_int("Stream", "TcpBitrate", c.tcp_bitrate, path.c_str());
    c.tcp_frame_rate = read_int("Stream", "TcpFrameRate", c.tcp_frame_rate, path.c_str());

    c.local_base_port = read_int("Network", "LocalBasePort", c.local_base_port, path.c_str());
    c.local_port_range = read_int("Network", "LocalPortRange", c.local_port_range, path.c_str());

    c.idle_timeout_sec = read_int("Advanced", "IdleTimeoutSec", c.idle_timeout_sec, path.c_str());
    c.iframe_interval = read_int("Advanced", "IFrameInterval", c.iframe_interval, path.c_str());
    c.max_h264_queue = read_int("Advanced", "MaxH264Queue", c.max_h264_queue, path.c_str());
    c.max_grpc_reconnect = read_int("Advanced", "MaxGrpcReconnect", c.max_grpc_reconnect, path.c_str());

    c.scroll_pixels_per_notch = read_int("Input", "ScrollPixelsPerNotch", c.scroll_pixels_per_notch, path.c_str());
    c.scroll_max = read_int("Input", "ScrollMax", c.scroll_max, path.c_str());
    c.audio_enabled = read_int("Audio", "Enabled", c.audio_enabled ? 1 : 0, path.c_str()) != 0;

    if (c.usb_scale < 1) c.usb_scale = 1;
    if (c.usb_scale > 5) c.usb_scale = 5;
    if (c.tcp_scale < 1) c.tcp_scale = 1;
    if (c.tcp_scale > 5) c.tcp_scale = 5;
    if (c.usb_frame_rate < 1) c.usb_frame_rate = 1;
    if (c.usb_frame_rate > 60) c.usb_frame_rate = 60;
    if (c.tcp_frame_rate < 1) c.tcp_frame_rate = 1;
    if (c.tcp_frame_rate > 60) c.tcp_frame_rate = 60;
    if (c.local_base_port < 1024) c.local_base_port = 10000;
    if (c.local_base_port > 60000) c.local_base_port = 10000;
    if (c.local_port_range < 10) c.local_port_range = 500;
    if (c.local_base_port + c.local_port_range > 65535) c.local_port_range = 65535 - c.local_base_port;
    if (c.idle_timeout_sec < 10) c.idle_timeout_sec = 300;
    if (c.idle_timeout_sec > 3600) c.idle_timeout_sec = 3600;
    if (c.max_h264_queue < 1) c.max_h264_queue = 10;
    if (c.max_h264_queue > 100) c.max_h264_queue = 100;
    if (c.max_grpc_reconnect < 1) c.max_grpc_reconnect = 3;
    if (c.max_grpc_reconnect > 20) c.max_grpc_reconnect = 20;

    LOGI("Config loaded from %s", path.c_str());
}

void config_save() {
    std::string path = ini_path();
    const AppConfig &c = g_config;

    write_int("Stream", "UsbScale", c.usb_scale, path.c_str());
    write_int("Stream", "UsbBitrate", c.usb_bitrate, path.c_str());
    write_int("Stream", "UsbFrameRate", c.usb_frame_rate, path.c_str());

    write_int("Stream", "TcpScale", c.tcp_scale, path.c_str());
    write_int("Stream", "TcpBitrate", c.tcp_bitrate, path.c_str());
    write_int("Stream", "TcpFrameRate", c.tcp_frame_rate, path.c_str());

    write_int("Network", "LocalBasePort", c.local_base_port, path.c_str());
    write_int("Network", "LocalPortRange", c.local_port_range, path.c_str());

    write_int("Advanced", "IdleTimeoutSec", c.idle_timeout_sec, path.c_str());
    write_int("Advanced", "IFrameInterval", c.iframe_interval, path.c_str());
    write_int("Advanced", "MaxH264Queue", c.max_h264_queue, path.c_str());
    write_int("Advanced", "MaxGrpcReconnect", c.max_grpc_reconnect, path.c_str());

    write_int("Input", "ScrollPixelsPerNotch", c.scroll_pixels_per_notch, path.c_str());
    write_int("Input", "ScrollMax", c.scroll_max, path.c_str());
    write_int("Audio", "Enabled", c.audio_enabled ? 1 : 0, path.c_str());

    LOGI("Config saved to %s", path.c_str());
}

} // namespace hscrcpy
