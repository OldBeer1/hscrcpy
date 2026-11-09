#ifndef HSCRCPY_AUDIO_PROTOCOL_H
#define HSCRCPY_AUDIO_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace hscrcpy::audio {
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kBits = 16;
constexpr uint32_t kBlockAlign = kChannels * kBits / 8;
constexpr size_t kTokenSize = 32;
constexpr size_t kHeaderSize = 20 + kTokenSize;
constexpr uint32_t kMaxFrameBytes = 19200; // At most 100 ms of PCM per packet.
constexpr size_t kMaxQueuedBytes = 15360;  // Bound queued audio to 80 ms.

inline uint32_t read_u32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void write_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (i * 8));
}
inline bool valid_token(const std::string &token) {
    if (token.size() != kTokenSize) return false;
    for (char c : token) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline std::array<uint8_t, kHeaderSize> make_header(const std::string &token) {
    std::array<uint8_t, kHeaderSize> h{};
    std::memcpy(h.data(), "HSA1", 4);
    write_u32(h.data() + 4, 1);
    write_u32(h.data() + 8, kSampleRate);
    write_u32(h.data() + 12, kChannels);
    write_u32(h.data() + 16, kBits);
    if (valid_token(token)) std::memcpy(h.data() + 20, token.data(), kTokenSize);
    return h;
}
inline bool valid_header(const uint8_t *h, const std::string &token) {
    return valid_token(token) && std::memcmp(h, "HSA1", 4) == 0 &&
           read_u32(h + 4) == 1 && read_u32(h + 8) == kSampleRate &&
           read_u32(h + 12) == kChannels && read_u32(h + 16) == kBits &&
           std::memcmp(h + 20, token.data(), kTokenSize) == 0;
}
inline bool valid_frame(uint32_t bytes) {
    return bytes > 0 && bytes <= kMaxFrameBytes && bytes % kBlockAlign == 0;
}
} // namespace hscrcpy::audio
#endif
