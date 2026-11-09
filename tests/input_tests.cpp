// SPDX-License-Identifier: Apache-2.0
// Input regression tests use a loopback agent and a fake HDC, never a phone.
#include "input_injector.h"
#include "input_gesture.h"
#include "config.h"
#include "nlohmann/json.hpp"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <future>
#include <limits>
#include <stdexcept>
#include <vector>

bool g_verbose = false;
using namespace std::chrono_literals;

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

namespace hscrcpy {
struct InputInjectorTestAccess {
    static void start(InputInjector &input, SOCKET sock) {
        input.control_sock_.store(static_cast<int>(sock));
        input.use_socket_mode_ = true;
        input.task_running_ = true;
        input.task_thread_ = std::thread(&InputInjector::task_loop, &input);
    }
    static void enqueue(InputInjector &input, std::function<void()> task) {
        std::lock_guard<std::mutex> lock(input.task_mutex_);
        input.queue_task_locked(std::move(task));
        input.task_cv_.notify_one();
    }
    static void join(InputInjector &input) {
        if (input.task_thread_.joinable()) input.task_thread_.join();
    }
    static void socket_mode(InputInjector &input, bool enabled) {
        input.use_socket_mode_ = enabled;
    }
};
}

struct Gate {
    std::promise<void> entered, released;
    std::shared_future<void> release = released.get_future().share();
    bool open = false;
    void unblock() { if (!open) { open = true; released.set_value(); } }
    ~Gate() { unblock(); }
};

struct Event { std::string api; uint32_t x, y; };
struct Fixture {
    hscrcpy::HdcWrapper hdc{"fake"};
    hscrcpy::InputInjector input{hdc};
    SOCKET peer = INVALID_SOCKET;
    std::atomic<bool> collecting{true};
    std::thread agent;
    std::mutex event_mutex;
    std::condition_variable event_cv;
    std::vector<Event> events;
    std::string agent_error;
    int ack_delay_ms;
    bool fragment_ack, omit_up_ack;
    Fixture(int delay_ms = 0, bool fragment = false, bool omit_up = false)
        : ack_delay_ms(delay_ms), fragment_ack(fragment), omit_up_ack(omit_up) {
        SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
        check(listener != INVALID_SOCKET, "listener");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "bind");
        check(listen(listener, 1) == 0, "listen");
        int len = sizeof(addr);
        getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len);
        SOCKET client = socket(AF_INET, SOCK_STREAM, 0);
        check(connect(client, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "connect");
        peer = accept(listener, nullptr, nullptr);
        closesocket(listener);
        check(peer != INVALID_SOCKET, "accept");
        int yes = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&yes), sizeof(yes));
        setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&yes), sizeof(yes));
        agent = std::thread([this] { collect(); });
        hscrcpy::InputInjectorTestAccess::start(input, client);
    }
    ~Fixture() {
        input.request_stop();
        hscrcpy::InputInjectorTestAccess::join(input);
        collecting = false;
        shutdown(peer, SD_BOTH);
        if (agent.joinable()) agent.join();
        closesocket(peer);
    }
    void reply(const std::string &data) {
        size_t offset = 0;
        while (offset < data.size()) {
            int n = send(peer, data.data() + offset, static_cast<int>(data.size() - offset), 0);
            check(n > 0, "agent reply send failed");
            offset += static_cast<size_t>(n);
        }
    }
    void collect() {
        try {
            std::string pending;
            while (collecting) {
                fd_set reads;
                FD_ZERO(&reads); FD_SET(peer, &reads);
                timeval timeout{0, 100000};
                if (select(0, &reads, nullptr, nullptr, &timeout) <= 0) continue;
                char data[4096];
                const int n = recv(peer, data, sizeof(data), 0);
                if (n <= 0) break;
                pending.append(data, n);
                size_t end = 0;
                int depth = 0;
                bool quoted = false, escaped = false;
                for (size_t i = 0; i < pending.size(); ++i) {
                    const char c = pending[i];
                    if (quoted) {
                        if (escaped) escaped = false;
                        else if (c == '\\') escaped = true;
                        else if (c == '"') quoted = false;
                    } else if (c == '"') quoted = true;
                    else if (c == '{') ++depth;
                    else if (c == '}' && --depth == 0) { end = i + 1; break; }
                }
                if (!end) continue;
                auto j = nlohmann::json::parse(pending.substr(0, end));
                // Mirror the real agent: requests coalesced in one receive lose
                // the trailing command. Serialization must prevent this.
                pending.clear();
                auto p = j.at("params");
                Event event{p.at("api"), p.at("args").at("x"), p.at("args").at("y")};
                {
                    std::lock_guard<std::mutex> lock(event_mutex);
                    events.push_back(event);
                }
                event_cv.notify_all();
                if (omit_up_ack && event.api == "touchUp") continue;
                if (ack_delay_ms) Sleep(ack_delay_ms);
                const std::string ack = "{\"result\":null,\"pts\":0}\n";
                if (fragment_ack) {
                    reply(ack.substr(0, 9));
                    Sleep(2);
                    reply(ack.substr(9, ack.size() - 10));
                    Sleep(2);
                    reply("\n");
                } else reply(ack);
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(event_mutex);
            agent_error = e.what();
            event_cv.notify_all();
        }
    }
    std::shared_ptr<Gate> block() {
        auto gate = std::make_shared<Gate>();
        auto entered = gate->entered.get_future();
        hscrcpy::InputInjectorTestAccess::enqueue(input, [gate] {
            gate->entered.set_value();
            gate->release.wait_for(5s);
        });
        check(entered.wait_for(2s) == std::future_status::ready, "worker gate timeout");
        return gate;
    }
    void flush() {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        hscrcpy::InputInjectorTestAccess::enqueue(input, [done] { done->set_value(); });
        check(future.wait_for(5s) == std::future_status::ready, "input queue timeout");
    }
    void wait_for_data() {
        std::unique_lock<std::mutex> lock(event_mutex);
        check(event_cv.wait_for(lock, 2s, [this] { return !events.empty() || !agent_error.empty(); }),
              "no first wheel event");
        check(agent_error.empty(), "agent receiver failed");
    }
    std::vector<Event> receive() {
        std::lock_guard<std::mutex> lock(event_mutex);
        check(agent_error.empty(), "agent receiver failed");
        std::vector<Event> result;
        result.swap(events);
        return result;
    }
};

static void check_swipe(const std::vector<Event> &events, bool decreasing_y) {
    check(events.size() >= 6, "swipe has no intermediate moves");
    check(events.front().api == "touchDown" && events.back().api == "touchUp", "swipe boundaries");
    for (size_t i = 1; i + 1 < events.size(); ++i) {
        check(events[i].api == "touchMove", "swipe events interleaved");
        check(decreasing_y ? events[i].y <= events[i-1].y : events[i].y >= events[i-1].y,
              "swipe moves backwards");
    }
}

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
    try {
        SetEnvironmentVariableA("HSCRCPY_AUDIO_TEST_LOG", "input-hdc.log");
        { std::ofstream clear("input-hdc.log", std::ios::trunc); }
        auto &config = hscrcpy::AppConfig::instance();
        config.scroll_pixels_per_notch = 200;
        config.scroll_max = 800;
        int passed = 0;
        {
            Fixture f;
            auto gate = f.block();
            f.input.queue_touch_down(200, 400);
            f.input.queue_touch_up(200, 400);
            check(f.receive().empty(), "touchUp bypassed queued touchDown");
            gate->unblock(); f.flush();
            auto events = f.receive();
            check(events.size() == 2 && events[0].api == "touchDown" &&
                  events[1].api == "touchUp", "tap must send one down then one up");
            ++passed;
        }
        {
            Fixture f;
            f.input.queue_touch_down(200, 400); f.input.queue_touch_up(200, 400);
            f.input.queue_touch_down(200, 400); f.input.queue_touch_up(200, 400);
            f.flush();
            auto events = f.receive();
            check(events.size() == 4 && events[0].api == "touchDown" &&
                  events[1].api == "touchUp" && events[2].api == "touchDown" &&
                  events[3].api == "touchUp", "second tap missing or reordered");
            ++passed;
        }
        {
            Fixture f;
            auto gate = f.block();
            f.input.queue_touch_down(300, 800);
            f.input.queue_touch_move(280, 700);
            f.input.queue_touch_move(240, 600);
            f.input.queue_touch_up(240, 590);
            // A new drag must not overwrite the first drag's queued move.
            f.input.queue_touch_down(500, 1000);
            f.input.queue_touch_move(450, 900);
            f.input.queue_touch_up(450, 900);
            gate->unblock(); f.flush();
            auto e = f.receive();
            check(e.size() == 7 && e[1].x == 240 && e[1].y == 600 &&
                  e[2].api == "touchMove" && e[2].y == 590 &&
                  e[3].api == "touchUp" && e[4].api == "touchDown" &&
                  e[5].x == 450, "queued drag lost or crossed gesture boundaries");
            ++passed;
        }
        {
            Fixture f;
            f.input.inject_wheel(300, 900, -120, 1600);
            // This sentinel executes only AFTER the whole swipe; nested enqueueing fails.
            auto gate = f.block();
            auto e = f.receive();
            check_swipe(e, true);
            check(e.front().y - e.back().y == 200, "wheel distance");
            gate->unblock();
            ++passed;
        }
        {
            Fixture f;
            f.input.inject_swipe(900, 1200, 100, 200, 40000);
            f.flush();
            auto e = f.receive();
            check_swipe(e, true);
            for (size_t i = 1; i < e.size(); ++i)
                check(e[i].x <= e[i-1].x && e[i].x >= 100 && e[i].y >= 200,
                      "up/left swipe coordinate underflow");
            check(e.back().x == 100 && e.back().y == 200, "swipe endpoint");
            ++passed;
        }
        {
            Fixture f;
            auto gate = f.block();
            for (int i = 0; i < 3; ++i) f.input.inject_wheel(300, 900, -40, 1600);
            gate->unblock(); f.flush();
            auto e = f.receive();
            check_swipe(e, true);
            check(e.front().y - e.back().y == 200, "high-resolution wheel multiplied distance");
            auto cancel = f.block();
            f.input.inject_wheel(300, 900, 120, 1600);
            f.input.inject_wheel(300, 900, -120, 1600);
            f.input.inject_wheel(300, 900, 0, 1600);
            cancel->unblock(); f.flush();
            check(f.receive().empty(), "opposite wheel deltas must cancel");
            ++passed;
        }
        {
            Fixture f;
            f.input.inject_wheel(300, 1200, -120, 2400);
            f.wait_for_data(); // the first touchDown has actually been sent
            f.input.inject_wheel(300, 1200, -120, 2400);
            f.flush();
            auto e = f.receive();
            check_swipe(e, true);
            check(e.front().y - e.back().y == 400,
                  "active wheel burst lost distance or restarted its touch");
            ++passed;
        }
        {
            Fixture f;
            f.input.inject_wheel(300, 1200, -120, 2400);
            f.wait_for_data();
            f.input.queue_touch_down(600, 300);
            f.input.queue_touch_up(600, 300);
            f.input.inject_wheel(300, 1200, -120, 2400);
            f.flush();
            auto e = f.receive();
            int down = 0, up = 0;
            size_t click = 0;
            for (size_t i = 0; i < e.size(); ++i) {
                if (e[i].api == "touchDown") {
                    ++down;
                    if (e[i].x == 600) { check(down == 2, "click reordered with wheel"); click = i; }
                }
                if (e[i].api == "touchUp") ++up;
            }
            check(down == 3 && up == 3 && click > 0 &&
                  e[click-1].api == "touchUp" && e[click+1].api == "touchUp",
                  "continuous wheel crossed an intervening click");
            ++passed;
        }
        {
            Fixture f;
            f.input.inject_wheel(300, 1200, -120, 2400);
            f.wait_for_data();
            f.input.inject_wheel(300, 1200, 120, 2400);
            f.flush();
            auto e = f.receive();
            check(e.front().api == "touchDown" && e.back().api == "touchUp" &&
                  e.front().y == e.back().y, "active wheel reversal lost net distance");
            ++passed;
        }
        {
            // Slow, fragmented acknowledgements must not let final move and
            // touchUp merge into a request the real agent silently truncates.
            Fixture f(20, true);
            f.input.queue_touch_down(300, 1000);
            f.input.queue_touch_move(600, 1000);
            f.input.queue_touch_up(900, 1000);
            f.flush();
            auto e = f.receive();
            check(e.size() == 4 && e[0].api == "touchDown" && e[1].api == "touchMove" &&
                  e[2].x == 900 && e[3].api == "touchUp" && e[3].x == 900,
                  "drag release lost with delayed/fragmented replies");
            ++passed;
        }
        {
            using hscrcpy::input::interpolate;
            check(interpolate(1200, 200, 1, 2) == 700, "signed interpolation");
            check(interpolate(0xffffffffu, 0u, 1, 2) == 0x80000000u, "wide interpolation");
            for (uint32_t height : {4u, 20u, 1600u}) {
                for (int64_t pixels : {int64_t{-800}, int64_t{800}, (std::numeric_limits<int64_t>::min)()}) {
                    auto s = hscrcpy::input::wheel_swipe(0, height - 1, pixels, height);
                    check(s.valid && s.from_y < height && s.to_y < height, "wheel outside screen");
                }
            }
            check(!hscrcpy::input::wheel_swipe(0, 0, 10, 0).valid, "zero-size screen");
            ++passed;
        }
        {
            std::ifstream log("input-hdc.log");
            std::string content((std::istreambuf_iterator<char>(log)), {});
            check(content.empty(), "successful socket touches duplicated through shell");
            ++passed;
        }
        {
            Fixture f;
            hscrcpy::InputInjectorTestAccess::socket_mode(f.input, false);
            f.input.queue_touch_down(100, 200);
            f.flush();
            hscrcpy::InputInjectorTestAccess::socket_mode(f.input, true);
            f.input.queue_touch_move(100, 180); f.input.queue_touch_up(100, 180);
            f.flush();
            check(f.receive().empty(), "gesture switched backend after shell touchDown");
            std::ifstream log("input-hdc.log");
            std::string content((std::istreambuf_iterator<char>(log)), {});
            check(content.find("uinput -T -d 100 200") != std::string::npos &&
                  content.find("uinput -T -m 100 200 100 180 16") != std::string::npos &&
                  content.find("uinput -T -u 100 180") != std::string::npos,
                  "shell gesture incomplete");
            ++passed;
        }
        {
            Fixture f(0, false, true);
            f.input.queue_touch_down(400, 900);
            f.input.queue_touch_move(800, 900);
            f.input.queue_touch_up(800, 900);
            f.flush();
            auto e = f.receive();
            check(e.size() == 3 && e.back().api == "touchUp", "missing-ACK release never sent");
            std::ifstream log("input-hdc.log");
            std::string content((std::istreambuf_iterator<char>(log)), {});
            check(content.find("uinput -T -u 800 900") != std::string::npos,
                  "missing release ACK must attempt shell recovery");
            ++passed;
        }
        std::printf("PASS: %d input regression scenarios (loopback/fake HDC, no phone)\n", passed);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        WSACleanup();
        return 1;
    }
    WSACleanup();
    return 0;
}
