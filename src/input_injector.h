#ifndef HSCRCPY_INPUT_INJECTOR_H
#define HSCRCPY_INPUT_INJECTOR_H

#include "hdc_wrapper.h"
#include <cstdint>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>

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
    bool inject_mouse_down(const std::string &button, uint32_t x, uint32_t y);
    bool inject_mouse_move(const std::string &button, uint32_t x, uint32_t y);
    bool inject_mouse_up(const std::string &button, uint32_t x, uint32_t y);
    bool inject_wheel(uint32_t x, uint32_t y, int32_t delta);
    bool inject_swipe(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, uint32_t velocity = 600);
    bool inject_back();
    bool inject_home();
    bool inject_power();
    bool inject_key_event(const std::string &key);
    bool inject_reboot();

private:
    bool send_json_command(const std::string &json);
    bool send_to_socket(const std::string &json);
    bool setup_control_port();
    bool push_agent_so();
    bool ensure_uitest_agent();
    void reconnect_loop();

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
};

} // namespace hscrcpy

#endif
