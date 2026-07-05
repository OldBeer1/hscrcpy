#ifndef HSCRCPY_INPUT_INJECTOR_H
#define HSCRCPY_INPUT_INJECTOR_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "hdc_wrapper.h"
#include <cstdint>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>
#include <queue>

namespace hscrcpy {

class InputInjector {
public:
    explicit InputInjector(HdcWrapper &hdc, uint16_t base_port = 0);
    ~InputInjector();

    bool init(const std::string &uitest_version = "");
    void cleanup();

    bool inject_click(uint32_t x, uint32_t y);
    bool inject_long_click(uint32_t x, uint32_t y);
    bool inject_touch_down(uint32_t x, uint32_t y);
    bool inject_touch_move(uint32_t x, uint32_t y);
    bool inject_touch_up(uint32_t x, uint32_t y);
    void queue_touch_down(uint32_t x, uint32_t y);
    void queue_touch_move(uint32_t x, uint32_t y);
    void queue_touch_up(uint32_t x, uint32_t y);
    bool inject_mouse_down(const std::string &button, uint32_t x, uint32_t y);
    bool inject_mouse_move(const std::string &button, uint32_t x, uint32_t y);
    bool inject_mouse_up(const std::string &button, uint32_t x, uint32_t y);
    bool inject_wheel(uint32_t x, uint32_t y, int32_t delta);
    bool inject_swipe(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, uint32_t velocity = 600);
    bool inject_back();
    bool inject_home();
    bool inject_recent();
    bool inject_power();
    bool inject_key_event(const std::string &key);
    bool inject_key_code(int key_code, bool shift = false);
    bool inject_text(const std::string &text, uint32_t x = 0, uint32_t y = 0);
    bool inject_reboot();

private:
    bool send_to_socket(const std::string &json);
    bool setup_control_port();
    bool push_agent_so();
    bool ensure_uitest_agent();
    void reconnect_loop();
    void task_loop();
    void shell_bg_loop();

    HdcWrapper &hdc_;
    uint16_t base_port_ = 0;
    uint16_t local_control_port_ = 0;
    bool use_abstract_control_ = false;
    std::atomic<bool> use_socket_mode_{false};
    std::atomic<int> control_sock_{-1};
    std::string uitest_version_;
    std::mutex socket_mutex_;
    std::atomic<bool> reconnect_running_{false};
    std::thread reconnect_thread_;
    // Last touch position for uinput -T -m (move needs from/to coordinates)
    std::atomic<uint32_t> last_touch_x_{0};
    std::atomic<uint32_t> last_touch_y_{0};

    std::chrono::steady_clock::time_point last_move_time_;
    // Move coalescing: only one move task in queue, uses latest coordinates.
    // pending_move_x_/y_ are only written from the UI thread (WM_MOUSEMOVE handler),
    // so no interleaving between writes. Task thread reads them atomically — at worst
    // it sees a slightly stale y from the previous store, which is harmless for move.
    std::atomic<bool> has_pending_move_{false};
    std::atomic<uint32_t> pending_move_x_{0};
    std::atomic<uint32_t> pending_move_y_{0};
    // Move generation: incremented on touchUp so stale queued moves self-cancel
    // without flushing the entire queue (which could drop touchDown events).
    std::atomic<uint64_t> move_generation_{0};
    std::queue<std::function<void()>> task_queue_;
    std::mutex task_mutex_;
    std::condition_variable task_cv_;
    std::atomic<bool> task_running_{false};
    std::thread task_thread_;

    // Background shell thread: runs touchUp shell fallback without blocking task_queue.
    // Ensures detached threads don't outlive this object (use-after-free fix).
    std::thread shell_bg_thread_;
    std::queue<std::string> shell_bg_queue_;
    std::mutex shell_bg_mutex_;
    std::condition_variable shell_bg_cv_;
    std::atomic<bool> shell_bg_running_{false};

};

} // namespace hscrcpy

#endif
