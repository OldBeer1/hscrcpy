#include "input_injector.h"
#include "input_gesture.h"
#include "nlohmann/json.hpp"
#include "config.h"
#include "log.h"
#include "net_util.h"
#include "protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
using socket_t = SOCKET;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCK (-1)
#define CLOSE_SOCKET close
#endif

namespace hscrcpy {

static bool is_local_port_free(uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCK) return false;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool free = (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CLOSE_SOCKET(s);
    return free;
}

InputInjector::InputInjector(HdcWrapper &hdc, uint16_t base_port) : hdc_(hdc), base_port_(base_port) {}

InputInjector::~InputInjector() {
    reconnect_running_ = false;
    task_running_ = false;
    task_cv_.notify_all();
    if (task_thread_.joinable()) task_thread_.join();
    if (reconnect_thread_.joinable()) reconnect_thread_.join();
    cleanup();
}

bool InputInjector::init(const std::string &uitest_version) {
    hdc_.ensure_connected();

    if (!uitest_version.empty()) {
        uitest_version_ = uitest_version;
        LOGI("Input: uitest version (shared): %s", uitest_version_.c_str());
    } else {
        std::string output;
        hdc_.shell_command("/system/bin/uitest --version", &output, 8);
        for (auto &c : output) {
            if ((c >= '0' && c <= '9') || c == '.') uitest_version_ += c;
            else if (!uitest_version_.empty()) break;
        }
        LOGI("Input: uitest version: %s", uitest_version_.c_str());
    }

    std::istringstream s1("6.0.2.1"), s2(uitest_version_);
    std::string p1, p2;
    while (std::getline(s1, p1, '.') && std::getline(s2, p2, '.')) {
        int n1 = std::atoi(p1.c_str());
        int n2 = std::atoi(p2.c_str());
        if (n2 != n1) break;
    }

    reconnect_running_ = true;
    bool agent_found = ensure_uitest_agent();

    if (!push_agent_so()) {
        LOGW("Failed to push agent SO, will use uitest uiInput shell commands");
        use_socket_mode_ = false;
    } else if (!agent_found) {
        // No abstract socket found — don't try TCP 8012 (unreliable on cloud phones,
        // e.g. port 8012 is a system service, not uitest agent).
        // reconnect_loop will find abstract socket and upgrade to socket mode.
        LOGW("uitest agent not available, using shell commands until reconnect");
        use_socket_mode_ = false;
    } else if (!setup_control_port()) {
        LOGW("Failed to setup control port, will use uitest uiInput shell commands");
        use_socket_mode_ = false;
    } else {
        use_socket_mode_ = true;
    }

    // Always start reconnect loop — it will find abstract socket and upgrade to socket mode
    reconnect_running_ = true;
    reconnect_thread_ = std::thread(&InputInjector::reconnect_loop, this);

    // Always start task thread — all inject methods use task_queue_
    task_running_ = true;
    task_thread_ = std::thread(&InputInjector::task_loop, this);

    LOGI("Input injector initialized (socket_mode=%s, abstract_control=%s)",
         use_socket_mode_ ? "true" : "false", use_abstract_control_ ? "true" : "false");
    return true;
}

void InputInjector::request_stop() {
    // Non-blocking: signal all threads to exit without joining them.
    // Used by WM_CLOSE to immediately wake blocked threads before stop_mirroring().
    reconnect_running_ = false;
    task_running_ = false;
    task_cv_.notify_all();
}

void InputInjector::cleanup() {
    reconnect_running_ = false;
    int sock = control_sock_.exchange(-1);
    if (sock >= 0) {
        CLOSE_SOCKET(static_cast<socket_t>(sock));
    }
    if (local_control_port_ > 0) {
        hdc_.forward_remove(local_control_port_, use_abstract_control_,
                            use_abstract_control_ ? kUitestSocketName : "");
    }
}

bool InputInjector::ensure_uitest_agent() {
    for (int i = 0; i < 10; i++) {
        if (!reconnect_running_) return false;
        std::string output;

        hdc_.shell_command("cat /proc/net/unix", &output, 3);
        if (!reconnect_running_) return false;
        if (output.find("uitest_socket") != std::string::npos) {
            LOGI("uitest agent listening on abstract socket uitest_socket (check %d)", i + 1);
            use_abstract_control_ = true;
            return true;
        }

        // TCP port 8012 detection disabled: port 8012 is not reliably uitest agent.
        // On some devices (e.g. cloud phones with uitest 5.0.x), port 8012 is a system service.
        // Only abstract socket mode is reliable for socket-based input injection.
        // Fall through to start uitest daemon and retry abstract socket detection.

        if (i == 0) {
            LOGI("uitest agent not found yet, starting uitest daemon...");
            hdc_.shell_command("uitest start-daemon singleness", nullptr, 5);
            if (!reconnect_running_) return false;
        }
        Sleep(300);
        if (!reconnect_running_) return false;
    }

    LOGW("uitest agent still not available after checks (no abstract socket found)");
    return false;
}

bool InputInjector::push_agent_so() {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string exe_dir(exe_path);
    auto last_sep = exe_dir.find_last_of("\\/");
    if (last_sep != std::string::npos) exe_dir = exe_dir.substr(0, last_sep + 1);
    std::string server_dir = exe_dir + "server";

    std::string agent_path;
    const char *agents[] = {
        "uitest_agent_1.2.2.so",
        "uitest_agent_1.1.10.so",
        "uitest_agent_1.1.5.so",
        "uitest_agent_1.1.3.so",
        "uitest_agent_x86_1.1.9.so",
    };
    for (auto &name : agents) {
        std::string path = server_dir + "\\" + name;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            agent_path = path;
            break;
        }
    }

    if (agent_path.empty()) {
        LOGW("No agent SO found");
        return false;
    }

    std::string check_output;
    hdc_.shell_command("ls -l /data/local/tmp/agent.so", &check_output, 3);
    if (check_output.find("No such file") == std::string::npos && !check_output.empty()) {
        LOGI("Agent already exists on device");
        return true;
    }

    LOGI("Pushing agent %s", agent_path.c_str());
    return hdc_.push_file(agent_path, kDeviceAgentPath);
}

bool InputInjector::setup_control_port() {
    uint16_t start_port = base_port_ > 0 ? base_port_ + 1 : AppConfig::instance().local_base_port + 1;
    uint16_t end_port = start_port + 5;

    for (uint16_t port = start_port; port < end_port; port++) {
        if (!reconnect_running_) return false;
        if (!is_local_port_free(port)) continue;

        bool ok;
        if (use_abstract_control_) {
            ok = hdc_.forward_localabstract(port, kUitestSocketName);
        } else {
            ok = hdc_.forward_tcp(port, kDefaultControlPort);
        }
        if (!ok) continue;
        if (!reconnect_running_) {
            hdc_.forward_remove(port, use_abstract_control_,
                                use_abstract_control_ ? kUitestSocketName : "");
            return false;
        }

        int sock = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
        if (sock == static_cast<int>(INVALID_SOCK)) {
            LOGE("Failed to create control socket");
            hdc_.forward_remove(port, use_abstract_control_,
                                use_abstract_control_ ? kUitestSocketName : "");
            continue;
        }

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(static_cast<socket_t>(sock),
                       reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
            LOGW("Forward OK but connect to control port %u failed (abstract=%s)",
                 port, use_abstract_control_ ? "true" : "false");
            CLOSE_SOCKET(static_cast<socket_t>(sock));
            hdc_.forward_remove(port, use_abstract_control_,
                                use_abstract_control_ ? kUitestSocketName : "");
            continue;
        }
        if (!reconnect_running_) {
            CLOSE_SOCKET(static_cast<socket_t>(sock));
            hdc_.forward_remove(port, use_abstract_control_,
                                use_abstract_control_ ? kUitestSocketName : "");
            return false;
        }

        // TCP_NODELAY: disable Nagle for low-latency touch event delivery
        int nodelay = 1;
        setsockopt(static_cast<socket_t>(sock), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));

        // TCP keepalive: detect silently dropped connections on TCP devices
        set_tcp_keepalive(sock);

        // Set send/recv timeouts for blocking mode
        DWORD timeout_ms = 3000;
        setsockopt(static_cast<socket_t>(sock), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(static_cast<socket_t>(sock), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));

        local_control_port_ = port;
        control_sock_.store(sock);
        LOGI("Control socket connected on port %u (abstract=%s)", local_control_port_,
             use_abstract_control_ ? "true" : "false");
        return true;
    }

    LOGE("Failed to setup control port forward after %d attempts", end_port - start_port);
    return false;
}

bool InputInjector::send_to_socket(const std::string &json) {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    const int sock = control_sock_.load();
    if (sock < 0) return false;
    const auto sock_fd = static_cast<socket_t>(sock);
    auto disconnect = [this, sock_fd]() {
        CLOSE_SOCKET(sock_fd);
        control_sock_.store(-1);
        return false;
    };
    size_t offset = 0;
    while (offset < json.size()) {
        int n = ::send(sock_fd, json.data() + offset, static_cast<int>(json.size() - offset), 0);
        if (n <= 0) {
            LOGW("Input send failed (err=%d)", WSAGetLastError());
            return disconnect();
        }
        offset += static_cast<size_t>(n);
    }

    // The agent consumes one JSON request per receive; concatenated requests can
    // silently lose touchUp. Keep exactly one request in flight until its reply.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    std::string reply;
    while (reply.size() < 8192) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        fd_set reads;
        FD_ZERO(&reads); FD_SET(sock_fd, &reads);
        timeval timeout{static_cast<long>(remaining / 1000000), static_cast<long>(remaining % 1000000)};
        if (select(0, &reads, nullptr, nullptr, &timeout) <= 0) break;
        char buffer[512];
        const int n = ::recv(sock_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            LOGW("Input reply connection closed (err=%d)", WSAGetLastError());
            return disconnect();
        }
        reply.append(buffer, n);
        // Consume the line terminator too; it may arrive in a separate TCP packet.
        if (reply.find('\n') == std::string::npos) continue;
        const auto response = nlohmann::json::parse(reply, nullptr, false);
        if (!response.is_discarded()) {
            if (!response.is_object() || !response.contains("result") ||
                (response.contains("exception") && !response["exception"].is_null()) ||
                (response.contains("error") && !response["error"].is_null())) {
                LOGW("Input agent rejected request: %.300s", reply.c_str());
                return disconnect();
            }
            return true;
        }
        if (reply.find('\n') != std::string::npos) {
            LOGW("Invalid input reply: %.300s", reply.c_str());
            return disconnect();
        }
    }
    LOGW("Input acknowledgement missing or oversized; reconnect required");
    return disconnect();
}

bool InputInjector::send_touch_json(const char *api, uint32_t x, uint32_t y) {
    char json[512];
    snprintf(json, sizeof(json),
        "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
        "\"params\":{\"api\":\"%s\",\"args\":{\"x\":%u,\"y\":%u}}}", api, x, y);
    return send_to_socket(json);
}

bool InputInjector::dispatch_touch_down(uint32_t x, uint32_t y, bool allow_shell) {
    if (touch_backend_ != TouchBackend::None)
        dispatch_touch_up(last_touch_x_.load(), last_touch_y_.load());
    bool sent = false;
    if (use_socket_mode_ && control_sock_.load() >= 0)
        sent = send_touch_json("touchDown", x, y);
    if (sent) touch_backend_ = TouchBackend::Socket;
    else if (allow_shell) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -d %u %u", x, y);
        sent = hdc_.shell_command(cmd, nullptr, 3);
        touch_backend_ = sent ? TouchBackend::Shell : TouchBackend::None;
    } else touch_backend_ = TouchBackend::None;
    if (sent) {
        last_touch_x_.store(x); last_touch_y_.store(y);
        touch_down_time_ = std::chrono::steady_clock::now();
    }
    return sent;
}

bool InputInjector::dispatch_touch_move(uint32_t x, uint32_t y) {
    if (touch_backend_ == TouchBackend::None) return false;
    const uint32_t from_x = last_touch_x_.load(), from_y = last_touch_y_.load();
    if (x == from_x && y == from_y) return true;
    bool sent;
    if (touch_backend_ == TouchBackend::Socket) sent = send_touch_json("touchMove", x, y);
    else {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -m %u %u %u %u 16", from_x, from_y, x, y);
        sent = hdc_.shell_command(cmd, nullptr, 3);
    }
    if (sent) { last_touch_x_.store(x); last_touch_y_.store(y); }
    return sent;
}

bool InputInjector::dispatch_touch_up(uint32_t x, uint32_t y) {
    if (touch_backend_ == TouchBackend::None) return true;
    // A fast drag must retain its final move, even if intermediate moves were coalesced.
    dispatch_touch_move(x, y);
    const auto elapsed = std::chrono::steady_clock::now() - touch_down_time_;
    const auto minimum = std::chrono::milliseconds(40);
    if (elapsed < minimum) std::this_thread::sleep_for(minimum - elapsed);
    bool sent = false;
    if (touch_backend_ == TouchBackend::Socket) sent = send_touch_json("touchUp", x, y);
    if (touch_backend_ == TouchBackend::Shell || !sent) {
        // Recovery only after a failed socket send. Never duplicate a successful release.
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -u %u %u", x, y);
        sent = hdc_.shell_command(cmd, nullptr, 3);
    }
    touch_backend_ = TouchBackend::None;
    return sent;
}

bool InputInjector::inject_click(uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this, x, y]() {
        if (use_socket_mode_ && control_sock_.load() >= 0) {
            if (dispatch_touch_down(x, y, false)) dispatch_touch_up(x, y);
        } else {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "uinput -T -c %u %u 40", x, y);
            hdc_.shell_command(cmd, nullptr, 3);
        }
    });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_long_click(uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this, x, y]() {
        if (use_socket_mode_ && control_sock_.load() >= 0) {
            if (dispatch_touch_down(x, y, false)) {
                Sleep(500);
                dispatch_touch_up(x, y);
            }
        } else {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "uitest uiInput longClick %u %u", x, y);
            hdc_.shell_command(cmd, nullptr, 5);
        }
    });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_touch_down(uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    pending_move_.reset();
    last_move_time_ = {};
    queue_task_locked([this, x, y]() { dispatch_touch_down(x, y); });
    task_cv_.notify_one();
    return true;
}
bool InputInjector::inject_touch_move(uint32_t x, uint32_t y) {
    queue_touch_move(x, y);
    return true;
}
bool InputInjector::inject_touch_up(uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    // The pending move cell stays alive in its queued task; a later gesture cannot overwrite it.
    pending_move_.reset();
    queue_task_locked([this, x, y]() { dispatch_touch_up(x, y); });
    task_cv_.notify_one();
    return true;
}
void InputInjector::queue_touch_down(uint32_t x, uint32_t y) { inject_touch_down(x, y); }
void InputInjector::queue_touch_up(uint32_t x, uint32_t y) { inject_touch_up(x, y); }

void InputInjector::queue_touch_move(uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (pending_move_ && pending_move_->queued) {
        pending_move_->x = x; pending_move_->y = y;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_move_time_ < std::chrono::milliseconds(16)) return;
    last_move_time_ = now;
    auto move = std::make_shared<PendingMove>();
    move->x = x; move->y = y;
    pending_move_ = move;
    queue_task_locked([this, move]() {
        uint32_t x, y;
        {
            std::lock_guard<std::mutex> lock(task_mutex_);
            x = move->x; y = move->y;
            move->queued = false;
        }
        dispatch_touch_move(x, y);
    });
    task_cv_.notify_one();
}

bool InputInjector::inject_mouse_down(const std::string &button, uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"mouseDown\",\"args\":{\"x\":%u,\"y\":%u,\"mouseType\":\"%s\"}}}",
                 x, y, button.c_str());
        std::string msg(json);
        queue_task_locked([this, msg, x, y]() {
            if (!send_to_socket(msg)) {
                LOGW("mouse_down socket failed, fallback to shell");
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "uinput -T -d %u %u", x, y);
                hdc_.shell_command(cmd, nullptr, 3);
            }
        });
    } else {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -d %u %u", x, y);
        std::string s(cmd);
        queue_task_locked([this, s]() { hdc_.shell_command(s, nullptr, 3); });
    }
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_mouse_move(const std::string &button, uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (use_socket_mode_) {
        char json[512];
        if (button.empty()) {
            snprintf(json, sizeof(json),
                     "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                     "\"params\":{\"api\":\"mouseMove\",\"args\":{\"x\":%u,\"y\":%u}}}",
                     x, y);
        } else {
            snprintf(json, sizeof(json),
                     "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                     "\"params\":{\"api\":\"mouseMove\",\"args\":{\"x\":%u,\"y\":%u,\"mouseType\":\"%s\"}}}",
                     x, y, button.c_str());
        }
        std::string msg(json);
        queue_task_locked([this, msg]() { send_to_socket(msg); });
    } else {
        uint32_t from_x = last_touch_x_.load(), from_y = last_touch_y_.load();
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -m %u %u %u %u", from_x, from_y, x, y);
        std::string s(cmd);
        queue_task_locked([this, s, x, y]() {
            hdc_.shell_command(s, nullptr, 3);
            last_touch_x_.store(x);
            last_touch_y_.store(y);
        });
    }
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_mouse_up(const std::string &button, uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"mouseUp\",\"args\":{\"x\":%u,\"y\":%u,\"mouseType\":\"%s\"}}}",
                 x, y, button.c_str());
        std::string msg(json);
        queue_task_locked([this, msg, x, y]() {
            if (!send_to_socket(msg)) {
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "uinput -T -u %u %u", x, y);
                hdc_.shell_command(cmd, nullptr, 3);
            }
        });
    } else {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -u %u %u", x, y);
        std::string s(cmd);
        queue_task_locked([this, s]() { hdc_.shell_command(s, nullptr, 3); });
    }
    task_cv_.notify_one();
    return true;
}

bool InputInjector::perform_swipe(uint32_t from_x, uint32_t from_y,
                                  uint32_t to_x, uint32_t to_y, uint32_t velocity) {
    velocity = std::clamp(velocity, 200u, 40000u);
    if (!use_socket_mode_ || control_sock_.load() < 0) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uitest uiInput swipe %u %u %u %u %u",
                 from_x, from_y, to_x, to_y, velocity);
        return hdc_.shell_command(cmd, nullptr, 5);
    }
    if (!dispatch_touch_down(from_x, from_y, false)) return false;
    const double distance = std::hypot(static_cast<double>(to_x) - from_x,
                                       static_cast<double>(to_y) - from_y);
    const int duration_ms = std::clamp(static_cast<int>(distance * 1000.0 / velocity), 40, 350);
    const int steps = std::clamp(duration_ms / 12, 4, 24);
    bool sent = true;
    for (int i = 1; i <= steps; ++i) {
        Sleep(static_cast<DWORD>(duration_ms / steps));
        if (!dispatch_touch_move(input::interpolate(from_x, to_x, i, steps),
                                 input::interpolate(from_y, to_y, i, steps))) {
            sent = false;
            break;
        }
    }
    // Release on this same worker, before any subsequent click or swipe can start.
    const bool released = dispatch_touch_up(to_x, to_y);
    return sent && released;
}

bool InputInjector::inject_swipe(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, uint32_t velocity) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this, from_x, from_y, to_x, to_y, velocity]() {
        perform_swipe(from_x, from_y, to_x, to_y, velocity);
    });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_back() {
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this]() { hdc_.shell_command("uinput -K -d 2 -u 2", nullptr, 3); });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_home() {
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this]() { hdc_.shell_command("uinput -K -d 1 -u 1", nullptr, 3); });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_recent() {
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this]() { hdc_.shell_command("uitest uiInput keyEvent 10011", nullptr, 3); });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_power() {
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this]() { hdc_.shell_command("uinput -K -d 18 -u 18", nullptr, 3); });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_key_event(const std::string &key) {
    std::string cmd = "uinput -K -d " + key + " -u " + key;
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this, cmd]() { hdc_.shell_command(cmd, nullptr, 3); });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_key_code(int key_code, bool shift) {
    char cmd[256];
    if (shift) {
        snprintf(cmd, sizeof(cmd), "uinput -K -d 2047 -d %d -u %d -u 2047", key_code, key_code);
    } else {
        snprintf(cmd, sizeof(cmd), "uinput -K -d %d -u %d", key_code, key_code);
    }
    LOGI("inject_key_code: %s", cmd);
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this, c = std::string(cmd)]() {
        hdc_.shell_command(c, nullptr, 3);
    });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_text(const std::string &text, uint32_t x, uint32_t y) {
    LOGI("inject_text: '%s' at (%u, %u)", text.c_str(), x, y);
    std::string escaped;
    for (char c : text) {
        if (c == '\'') escaped += "'\\''";
        else escaped += c;
    }
    char cmd[512];
    if (x > 0 || y > 0) {
        snprintf(cmd, sizeof(cmd), "uitest uiInput inputText %u %u '%s'", x, y, escaped.c_str());
    } else {
        snprintf(cmd, sizeof(cmd), "uitest uiInput inputText 0 0 '%s'", escaped.c_str());
    }
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this, cmd]() {
        hdc_.shell_command(cmd, nullptr, 5);
    });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_reboot() {
    std::lock_guard<std::mutex> lock(task_mutex_);
    queue_task_locked([this]() { hdc_.shell_command("reboot", nullptr, 5); });
    task_cv_.notify_one();
    return true;
}

// Caller holds task_mutex_. A later non-wheel action seals the current wheel
// burst so new wheel events cannot jump ahead of a click/key already in the queue.
void InputInjector::queue_task_locked(std::function<void()> task) {
    if (pending_wheel_) {
        pending_wheel_->accepting = false;
        pending_wheel_.reset();
    }
    task_queue_.push(std::move(task));
}

namespace {
// A private high-resolution timer avoids rounded Sleep intervals without
// changing the system timer resolution. The fallback also supports older Windows.
class WheelTick {
public:
    WheelTick() : timer_(CreateWaitableTimerExW(nullptr, nullptr, 0x00000002,
                                               TIMER_MODIFY_STATE | SYNCHRONIZE)) {}
    ~WheelTick() { if (timer_) CloseHandle(timer_); }
    void wait_until(std::chrono::steady_clock::time_point deadline) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= decltype(remaining)::zero()) return;
        LARGE_INTEGER due{};
        due.QuadPart = -(std::max)(int64_t{1},
            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) &&
            WaitForSingleObject(timer_, 100) == WAIT_OBJECT_0) return;
        std::this_thread::sleep_until(deadline);
    }
private:
    HANDLE timer_ = nullptr;
};
}

void InputInjector::perform_wheel(const std::shared_ptr<PendingWheel> &wheel) {
    uint32_t x, anchor_y, height;
    int64_t remaining;
    bool socket_mode;
    auto close_burst = [this, &wheel]() {
        wheel->accepting = false; // caller holds task_mutex_
        if (pending_wheel_ == wheel) pending_wheel_.reset();
    };
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        x = wheel->x; anchor_y = wheel->y; height = wheel->height;
        remaining = wheel->pixels;
        wheel->pixels = 0;
        if (remaining == 0) { close_burst(); return; }
        socket_mode = use_socket_mode_ && control_sock_.load() >= 0;
        if (!socket_mode) close_burst(); // shell swipe is an indivisible command
    }
    auto first = input::wheel_swipe(x, anchor_y, remaining, height);
    if (!first.valid) return;
    if (!socket_mode) {
        perform_swipe(x, first.from_y, x, first.to_y, 3000);
        return;
    }
    if (!dispatch_touch_down(x, first.from_y, false)) {
        std::lock_guard<std::mutex> lock(task_mutex_);
        close_burst();
        return;
    }

    const int64_t margin = (std::min)(int64_t{12}, static_cast<int64_t>(height) / 8);
    const int64_t low = margin, high = static_cast<int64_t>(height) - 1 - margin;
    const int64_t maximum = (std::min)(high - low, (std::max)(int64_t{1},
        static_cast<int64_t>(AppConfig::instance().scroll_max)));
    remaining = std::clamp(remaining, -maximum, maximum);
    int64_t y = first.from_y;
    WheelTick timer;
    auto next_tick = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
    auto idle_since = std::chrono::steady_clock::time_point{};
    while (task_running_) {
        timer.wait_until(next_tick);
        const auto now = std::chrono::steady_clock::now();
        next_tick = now + std::chrono::milliseconds(8);
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(task_mutex_);
            remaining = std::clamp(remaining + wheel->pixels, -maximum, maximum);
            wheel->pixels = 0;
            if (remaining == 0) {
                if (idle_since == std::chrono::steady_clock::time_point{}) idle_since = now;
                if (!wheel->accepting || now - idle_since >= std::chrono::milliseconds(24)) {
                    close_burst();
                    done = true;
                }
            } else idle_since = {};
        }
        if (done) break;
        if (remaining == 0) continue;
        // Small increments track the wheel smoothly; a fast burst catches up
        // adaptively instead of leaving a long queue behind the pointer.
        const int64_t magnitude = remaining < 0 ? -remaining : remaining;
        const int64_t step = (std::min)(magnitude, (std::max)(int64_t{24}, magnitude / 4));
        const int64_t next_y = std::clamp(y + (remaining > 0 ? step : -step), low, high);
        if (next_y == y) {
            // Reposition a lifted finger only when the screen edge is reached.
            dispatch_touch_up(x, static_cast<uint32_t>(y));
            auto rebased = input::wheel_swipe(x, anchor_y, remaining, height);
            if (!rebased.valid || !dispatch_touch_down(x, rebased.from_y, false)) break;
            y = rebased.from_y;
            continue;
        }
        if (!dispatch_touch_move(x, static_cast<uint32_t>(next_y))) break;
        remaining -= next_y - y;
        y = next_y;
    }
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        close_burst();
    }
    dispatch_touch_up(x, static_cast<uint32_t>(y));
}

bool InputInjector::inject_wheel(uint32_t x, uint32_t y, int32_t delta, uint32_t screen_height) {
    if (delta == 0 || screen_height < 4) return true;
    std::lock_guard<std::mutex> lock(task_mutex_);
    const int64_t pixels_per_notch = std::clamp(AppConfig::instance().scroll_pixels_per_notch, 1, 10000);
    const int64_t scaled = static_cast<int64_t>(delta) * pixels_per_notch + wheel_remainder_;
    const int64_t pixels = scaled / 120;
    wheel_remainder_ = scaled % 120;
    if (pixels == 0) return true;
    const int64_t maximum = (std::max)(int64_t{1}, static_cast<int64_t>(AppConfig::instance().scroll_max));
    if (pending_wheel_ && pending_wheel_->accepting) {
        pending_wheel_->pixels = std::clamp(pending_wheel_->pixels + pixels, -maximum, maximum);
        return true;
    }
    auto wheel = std::make_shared<PendingWheel>();
    wheel->x = x; wheel->y = y; wheel->height = screen_height;
    wheel->pixels = std::clamp(pixels, -maximum, maximum);
    pending_wheel_ = wheel;
    task_queue_.push([this, wheel]() { perform_wheel(wheel); });
    task_cv_.notify_one();
    return true;
}

void InputInjector::reconnect_loop() {
    LOGI("Reconnect loop started");
    int consecutive_failures = 0;
    while (reconnect_running_) {
        Sleep(2000);
        if (!reconnect_running_) break;

        // send_to_socket owns each reply. Draining here could steal the ACK
        // of a request in flight and leave a drag without its release.
        if (control_sock_.load() >= 0) {
            consecutive_failures = 0;
            continue;
        }
        LOGI("Control socket disconnected, attempting reconnect...");

        if (!ensure_uitest_agent()) {
            LOGW("Reconnect: uitest agent not available");
            consecutive_failures++;
            if (consecutive_failures >= 5) {
                LOGW("uitest agent unavailable %d times, killing and restarting", consecutive_failures);
                hdc_.shell_command("pkill -9 -f uitest", nullptr, 3);
                Sleep(1000);
                hdc_.shell_command("uitest start-daemon singleness", nullptr, 5);
                Sleep(500);
                consecutive_failures = 0;
            }
            continue;
        }

        int old_sock = control_sock_.exchange(-1);
        uint16_t old_port = local_control_port_;
        local_control_port_ = 0;

        if (old_sock >= 0) {
            CLOSE_SOCKET(static_cast<socket_t>(old_sock));
        }
        if (old_port > 0) {
            hdc_.forward_remove(old_port, use_abstract_control_,
                                use_abstract_control_ ? kUitestSocketName : "");
        }

        if (setup_control_port()) {
            LOGI("Control socket reconnected on port %u", local_control_port_);
            use_socket_mode_ = true;
            consecutive_failures = 0;
        } else {
            LOGW("Reconnect: setup_control_port failed");
            consecutive_failures++;
            if (consecutive_failures >= 5) {
                LOGW("Control socket reconnect failed %d times, killing uitest and restarting", consecutive_failures);
                hdc_.shell_command("pkill -9 -f uitest", nullptr, 3);
                Sleep(1000);
                hdc_.shell_command("uitest start-daemon singleness", nullptr, 5);
                Sleep(500);
                consecutive_failures = 0;
            }
        }
    }
    LOGI("Reconnect loop ended");
}

void InputInjector::task_loop() {
    while (task_running_) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(task_mutex_);
            task_cv_.wait_for(lock, std::chrono::milliseconds(16), [this] {
                return !task_queue_.empty() || !task_running_;
            });
            if (task_queue_.empty()) continue;
            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        // Drop queued inject commands while the device is unreachable.
        // No polling here: socket-mode tasks fail fast in send_to_socket (no hdc
        // round-trip), and shell-mode fallback only runs when socket failed —
        // which is exactly when a reachability probe is warranted.

        if (!use_socket_mode_) {
            // Shell-only mode: probe reachability, but only after a task was
            // actually dequeued (event-driven, not per-second polling). Cache the
            // result briefly to avoid hammering hdc on burst input.
            auto now = std::chrono::steady_clock::now();
            if (now - last_reach_check_ > std::chrono::milliseconds(2000)) {
                last_reach_check_ = now;
                device_reachable_ = hdc_.is_device_reachable();
            }
            if (!device_reachable_) {
                LOGW("Input task skipped: device unreachable");
                continue;
            }
        }

        task();
    }
    if (touch_backend_ != TouchBackend::None)
        dispatch_touch_up(last_touch_x_.load(), last_touch_y_.load());
}

} // namespace hscrcpy
