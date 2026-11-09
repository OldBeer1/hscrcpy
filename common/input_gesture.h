// SPDX-License-Identifier: Apache-2.0
#ifndef HSCRCPY_INPUT_GESTURE_H
#define HSCRCPY_INPUT_GESTURE_H
#include <algorithm>
#include <cstdint>
namespace hscrcpy::input {
inline uint32_t interpolate(uint32_t from, uint32_t to, int step, int steps) {
    if (steps <= 0) return to;
    step = std::clamp(step, 0, steps);
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return static_cast<uint32_t>(static_cast<int64_t>(from) + delta * step / steps);
}
struct VerticalSwipe {
    uint32_t x = 0, from_y = 0, to_y = 0;
    bool valid = false;
};
inline VerticalSwipe wheel_swipe(uint32_t x, uint32_t y, int64_t pixels, uint32_t height) {
    if (height < 4 || pixels == 0) return {};
    const int64_t margin = (std::min)(int64_t{12}, static_cast<int64_t>(height) / 8);
    const int64_t low = margin, high = static_cast<int64_t>(height) - 1 - margin;
    const uint64_t magnitude = pixels < 0 ? static_cast<uint64_t>(-(pixels + 1)) + 1 :
                                           static_cast<uint64_t>(pixels);
    const int64_t distance = static_cast<int64_t>((std::min)(magnitude, static_cast<uint64_t>(high - low)));
    if (distance == 0) return {};
    int64_t from = std::clamp(static_cast<int64_t>(y), low, high);
    if (pixels > 0) from = (std::min)(from, high - distance);
    else from = (std::max)(from, low + distance);
    return {x, static_cast<uint32_t>(from),
            static_cast<uint32_t>(from + (pixels > 0 ? distance : -distance)), true};
}
} // namespace hscrcpy::input
#endif
