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
#include <memory>
#include <chrono>

namespace hscrcpy {

class InputInjector {
public:
    explicit InputInjector(HdcWrapper &hdc, uint16_t base_port = 0);
    ~InputInjector();

    bool init(const std::string &uitest_version = "");
    void cleanup();
    void request_stop();  // Non-blocking: signal threads to exit

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
    bool inject_wheel(uint32_t x, uint32_t y, int32_t delta, uint32_t screen_height);
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
    void queue_task_locked(std::function<void()> task);
    friend struct InputInjectorTestAccess;
    bool send_touch_json(const char *api, uint32_t x, uint32_t y);
    bool dispatch_touch_down(uint32_t x, uint32_t y, bool allow_shell = true);
    bool dispatch_touch_move(uint32_t x, uint32_t y);
    bool dispatch_touch_up(uint32_t x, uint32_t y);
    bool perform_swipe(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, uint32_t velocity);

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

    enum class TouchBackend { None, Socket, Shell };
    TouchBackend touch_backend_ = TouchBackend::None; // input worker only
    std::chrono::steady_clock::time_point touch_down_time_{};
    struct PendingMove { uint32_t x = 0, y = 0; bool queued = true; };
    struct PendingWheel {
        uint32_t x = 0, y = 0, height = 0;
        int64_t pixels = 0;
        bool accepting = true;
    };
    void perform_wheel(const std::shared_ptr<PendingWheel> &wheel);
    // Queue cells are protected by task_mutex_; each gesture owns its own move cell.
    std::shared_ptr<PendingMove> pending_move_;
    std::shared_ptr<PendingWheel> pending_wheel_;
    int64_t wheel_remainder_ = 0;
    std::chrono::steady_clock::time_point last_move_time_{};
    std::queue<std::function<void()>> task_queue_;
    std::mutex task_mutex_;
    std::condition_variable task_cv_;
    std::atomic<bool> task_running_{false};
    std::thread task_thread_;
    // Cached device reachability for task_loop: avoids a 3s hdc echo per task.
    std::chrono::steady_clock::time_point last_reach_check_{};
    bool device_reachable_ = true;


};

} // namespace hscrcpy

#endif
