#ifndef HSCRCPY_COMMON_PROTOCOL_H
#define HSCRCPY_COMMON_PROTOCOL_H

#include <cstdint>

namespace hscrcpy {

constexpr uint16_t kDefaultVideoPort  = 5000;
constexpr uint16_t kDefaultControlPort = 8012;
constexpr uint16_t kDefaultLocalBasePort = 9900;

constexpr uint32_t kDefaultWidth   = 1260;
constexpr uint32_t kDefaultHeight  = 2720;
constexpr uint32_t kDefaultFps     = 30;
constexpr uint32_t kDefaultBitrate = 4000000;

constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;
constexpr int kVersionPatch = 0;

struct CaptureConfig {
    int scale = 1;
    int bit_rate = 31457280;
    int frame_rate = 30;
    int iframe_interval = 2000;
};

constexpr const char *kDeviceSoPath = "/data/local/tmp/libscreen_casting.z.so";
constexpr const char *kDeviceAgentPath = "/data/local/tmp/agent.so";
constexpr const char *kGrpcSocketName = "scrcpy_grpc_socket";
constexpr const char *kUitestSocketName = "uitest_socket";

inline uint16_t hton16(uint16_t x) {
    return static_cast<uint16_t>(((x & 0x00FFu) << 8) | ((x & 0xFF00u) >> 8));
}

inline uint32_t hton32(uint32_t x) {
    return (((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
            ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24));
}

inline uint16_t ntoh16(uint16_t x) { return hton16(x); }
inline uint32_t ntoh32(uint32_t x) { return hton32(x); }

inline uint64_t hton64(uint64_t x) {
    return ((static_cast<uint64_t>(hton32(static_cast<uint32_t>(x))) << 32) |
            static_cast<uint64_t>(hton32(static_cast<uint32_t>(x >> 32))));
}

inline uint64_t ntoh64(uint64_t x) { return hton64(x); }

} // namespace hscrcpy

#endif
