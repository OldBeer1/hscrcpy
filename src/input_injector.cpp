#include "input_injector.h"
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

    if (!ensure_uitest_agent()) {
        LOGW("uitest agent may not be available");
    }

    if (!push_agent_so()) {
        LOGW("Failed to push agent SO, will use uitest uiInput shell commands");
        use_socket_mode_ = false;
    } else if (!setup_control_port()) {
        LOGW("Failed to setup control port, will use uitest uiInput shell commands");
        use_socket_mode_ = false;
    } else {
        use_socket_mode_ = true;
        reconnect_running_ = true;
        reconnect_thread_ = std::thread(&InputInjector::reconnect_loop, this);
    }

    // Always start task thread — all inject methods use task_queue_
    task_running_ = true;
    task_thread_ = std::thread(&InputInjector::task_loop, this);

    LOGI("Input injector initialized (socket_mode=%s, abstract_control=%s)",
         use_socket_mode_ ? "true" : "false", use_abstract_control_ ? "true" : "false");
    return true;
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
        std::string output;

        hdc_.shell_command("cat /proc/net/unix", &output, 3);
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
        }
        Sleep(300);
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
        if (!is_local_port_free(port)) continue;

        bool ok;
        if (use_abstract_control_) {
            ok = hdc_.forward_localabstract(port, kUitestSocketName);
        } else {
            ok = hdc_.forward_tcp(port, kDefaultControlPort);
        }
        if (!ok) continue;

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
    int sock = control_sock_.load();
    if (sock < 0) {
        return false;
    }

    auto sock_fd = static_cast<socket_t>(sock);

    const char *data = json.c_str();
    int remaining = static_cast<int>(json.size());
    int total_sent = 0;

    while (remaining > 0) {
        int n = ::send(sock_fd, data + total_sent, remaining, 0);
        if (n > 0) {
            total_sent += n;
            remaining -= n;
            continue;
        }

        if (n == 0) {
            LOGW("send to socket: connection closed");
            CLOSE_SOCKET(sock_fd);
            control_sock_.store(-1);
            return false;
        }

        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            Sleep(1);
            continue;
        }
        LOGW("send to socket failed (err=%d), closing socket for reconnect", err);
        CLOSE_SOCKET(sock_fd);
        control_sock_.store(-1);
        return false;
    }

    return true;
}

bool InputInjector::inject_click(uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.push([this, x, y]() {
        if (use_socket_mode_) {
            char json[512];
            snprintf(json, sizeof(json),
                     "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                     "\"params\":{\"api\":\"touchDown\",\"args\":{\"x\":%u,\"y\":%u}}}",
                     x, y);
            send_to_socket(json);
            Sleep(10);
            snprintf(json, sizeof(json),
                     "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                     "\"params\":{\"api\":\"touchUp\",\"args\":{\"x\":%u,\"y\":%u}}}",
                     x, y);
            if (send_to_socket(json)) return;
        }
        // Shell fallback
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -d %u %u", x, y);
        hdc_.shell_command(cmd, nullptr, 3);
        snprintf(cmd, sizeof(cmd), "uinput -T -u %u %u", x, y);
        hdc_.shell_command(cmd, nullptr, 3);
    });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_long_click(uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.push([this, x, y]() {
        if (use_socket_mode_) {
            char json[512];
            snprintf(json, sizeof(json),
                     "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                     "\"params\":{\"api\":\"touchDown\",\"args\":{\"x\":%u,\"y\":%u}}}",
                     x, y);
            send_to_socket(json);
            Sleep(500);
            snprintf(json, sizeof(json),
                     "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                     "\"params\":{\"api\":\"touchUp\",\"args\":{\"x\":%u,\"y\":%u}}}",
                     x, y);
            if (send_to_socket(json)) return;
        }
        // Shell fallback
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -d %u %u", x, y);
        hdc_.shell_command(cmd, nullptr, 3);
        Sleep(500);
        snprintf(cmd, sizeof(cmd), "uinput -T -u %u %u", x, y);
        hdc_.shell_command(cmd, nullptr, 3);
    });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_touch_down(uint32_t x, uint32_t y) {
    last_touch_x_.store(x);
    last_touch_y_.store(y);
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"touchDown\",\"args\":{\"x\":%u,\"y\":%u}}}",
                 x, y);
        std::string msg(json);
        task_queue_.push([this, msg, x, y]() {
            if (!send_to_socket(msg)) {
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "uinput -T -d %u %u", x, y);
                hdc_.shell_command(cmd, nullptr, 3);
            }
        });
    } else {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -d %u %u", x, y);
        std::string s(cmd);
        task_queue_.push([this, s]() { hdc_.shell_command(s, nullptr, 3); });
    }
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_touch_move(uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"touchMove\",\"args\":{\"x\":%u,\"y\":%u}}}",
                 x, y);
        std::string msg(json);
        task_queue_.push([this, msg, x, y]() {
            if (send_to_socket(msg)) {
                last_touch_x_.store(x);
                last_touch_y_.store(y);
                return;
            }
            // Shell fallback
            uint32_t from_x = last_touch_x_.load(), from_y = last_touch_y_.load();
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "uinput -T -m %u %u %u %u", from_x, from_y, x, y);
            hdc_.shell_command(cmd, nullptr, 3);
            last_touch_x_.store(x);
            last_touch_y_.store(y);
        });
    } else {
        // Shell fallback: uinput -T -m needs from/to coordinates
        uint32_t from_x = last_touch_x_.load(), from_y = last_touch_y_.load();
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -m %u %u %u %u", from_x, from_y, x, y);
        std::string s(cmd);
        task_queue_.push([this, s, x, y]() {
            hdc_.shell_command(s, nullptr, 3);
            last_touch_x_.store(x);
            last_touch_y_.store(y);
        });
    }
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_touch_up(uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"touchUp\",\"args\":{\"x\":%u,\"y\":%u}}}",
                 x, y);
        std::string msg(json);
        task_queue_.push([this, msg, x, y]() {
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
        task_queue_.push([this, s]() { hdc_.shell_command(s, nullptr, 3); });
    }
    task_cv_.notify_one();
    return true;
}

void InputInjector::queue_touch_down(uint32_t x, uint32_t y) {
    last_touch_x_.store(x);
    last_touch_y_.store(y);
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"touchDown\",\"args\":{\"x\":%u,\"y\":%u}}}",
                 x, y);
        std::string msg(json);
        task_queue_.push([this, msg, x, y]() {
            if (!send_to_socket(msg)) {
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "uinput -T -d %u %u", x, y);
                hdc_.shell_command(cmd, nullptr, 3);
            }
        });
    } else {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -d %u %u", x, y);
        std::string s(cmd);
        task_queue_.push([this, s]() { hdc_.shell_command(s, nullptr, 3); });
    }
    task_cv_.notify_one();
}

void InputInjector::queue_touch_move(uint32_t x, uint32_t y) {
    // Throttle: limit move events to ~16ms intervals (~60Hz).
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_move_time_).count();
    if (elapsed_ms < 16) return;
    last_move_time_ = now;

    std::lock_guard<std::mutex> lock(task_mutex_);
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"touchMove\",\"args\":{\"x\":%u,\"y\":%u}}}",
                 x, y);
        std::string msg(json);
        task_queue_.push([this, msg, x, y]() {
            if (send_to_socket(msg)) {
                last_touch_x_.store(x);
                last_touch_y_.store(y);
                return;
            }
            // Shell fallback
            uint32_t from_x = last_touch_x_.load(), from_y = last_touch_y_.load();
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "uinput -T -m %u %u %u %u", from_x, from_y, x, y);
            hdc_.shell_command(cmd, nullptr, 3);
            last_touch_x_.store(x);
            last_touch_y_.store(y);
        });
    } else {
        uint32_t from_x = last_touch_x_.load(), from_y = last_touch_y_.load();
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -m %u %u %u %u", from_x, from_y, x, y);
        std::string s(cmd);
        task_queue_.push([this, s, x, y]() {
            hdc_.shell_command(s, nullptr, 3);
            last_touch_x_.store(x);
            last_touch_y_.store(y);
        });
    }
    task_cv_.notify_one();
}

void InputInjector::queue_touch_up(uint32_t x, uint32_t y) {
    std::lock_guard<std::mutex> lock(task_mutex_);

    // Flush stale queued moves
    while (!task_queue_.empty()) {
        task_queue_.pop();
    }

    // Always send touchUp via both socket AND shell.
    // Socket may silently fail (agent not processing, TCP buffering, etc).
    // Shell command via hdc is the guaranteed delivery path.
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"touchUp\",\"args\":{\"x\":%u,\"y\":%u}}}",
                 x, y);
        std::string msg(json);
        // Try socket first (fast, ~1ms)
        std::unique_lock<std::mutex> sock_lock(socket_mutex_, std::try_to_lock);
        if (sock_lock.owns_lock()) {
            int sock = control_sock_.load();
            if (sock >= 0) {
                auto sock_fd = static_cast<socket_t>(sock);
                const char *data = msg.c_str();
                int remaining = static_cast<int>(msg.size());
                int total_sent = 0;
                while (remaining > 0) {
                    int n = ::send(sock_fd, data + total_sent, remaining, 0);
                    if (n > 0) { total_sent += n; remaining -= n; continue; }
                    if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) break;
                    Sleep(1);
                }
                if (remaining == 0) {
                    LOGI("touchUp(%u,%u) socket sent", x, y);
                }
            }
            sock_lock.unlock();
        }
        // ALSO queue shell fallback as guaranteed delivery
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -u %u %u", x, y);
        std::string s(cmd);
        task_queue_.push([this, s]() { hdc_.shell_command(s, nullptr, 3); });
        task_cv_.notify_one();
    } else {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -u %u %u", x, y);
        std::string s(cmd);
        task_queue_.push([this, s]() { hdc_.shell_command(s, nullptr, 3); });
        task_cv_.notify_one();
    }
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
        task_queue_.push([this, msg, x, y]() {
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
        task_queue_.push([this, s]() { hdc_.shell_command(s, nullptr, 3); });
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
        task_queue_.push([this, msg]() { send_to_socket(msg); });
    } else {
        uint32_t from_x = last_touch_x_.load(), from_y = last_touch_y_.load();
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uinput -T -m %u %u %u %u", from_x, from_y, x, y);
        std::string s(cmd);
        task_queue_.push([this, s, x, y]() {
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
        task_queue_.push([this, msg, x, y]() {
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
        task_queue_.push([this, s]() { hdc_.shell_command(s, nullptr, 3); });
    }
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_swipe(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, uint32_t velocity) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.push([this, from_x, from_y, to_x, to_y, velocity]() {
        if (use_socket_mode_) {
            char json[512];
            snprintf(json, sizeof(json),
                     "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                     "\"params\":{\"api\":\"touchDown\",\"args\":{\"x\":%u,\"y\":%u}}}",
                     from_x, from_y);
            send_to_socket(json);
            Sleep(20);
            int steps = 10;
            for (int i = 1; i <= steps; i++) {
                uint32_t cx = from_x + (to_x - from_x) * i / steps;
                uint32_t cy = from_y + (to_y - from_y) * i / steps;
                snprintf(json, sizeof(json),
                         "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                         "\"params\":{\"api\":\"touchMove\",\"args\":{\"x\":%u,\"y\":%u}}}",
                         cx, cy);
                send_to_socket(json);
                Sleep(velocity / steps / 2);
            }
            snprintf(json, sizeof(json),
                     "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                     "\"params\":{\"api\":\"touchUp\",\"args\":{\"x\":%u,\"y\":%u}}}",
                     to_x, to_y);
            send_to_socket(json);
            return;
        }
        // Shell fallback
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "uitest uiInput swipe %u %u %u %u %u", from_x, from_y, to_x, to_y, velocity);
        hdc_.shell_command(cmd, nullptr, 5);
    });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_back() {
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.push([this]() { hdc_.shell_command("uinput -K -d 2 -u 2", nullptr, 3); });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_home() {
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.push([this]() { hdc_.shell_command("uinput -K -d 1 -u 1", nullptr, 3); });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_recent() {
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.push([this]() { hdc_.shell_command("uitest uiInput keyEvent 10011", nullptr, 3); });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_power() {
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.push([this]() { hdc_.shell_command("uinput -K -d 18 -u 18", nullptr, 3); });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_key_event(const std::string &key) {
    std::string cmd = "uinput -K -d " + key + " -u " + key;
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.push([this, cmd]() { hdc_.shell_command(cmd, nullptr, 3); });
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
    task_queue_.push([this, c = std::string(cmd)]() {
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
    task_queue_.push([this, cmd]() {
        hdc_.shell_command(cmd, nullptr, 5);
    });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_reboot() {
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.push([this]() { hdc_.shell_command("reboot", nullptr, 5); });
    task_cv_.notify_one();
    return true;
}

bool InputInjector::inject_wheel(uint32_t x, uint32_t y, int32_t delta) {
    int notches = std::abs(delta) / 120;
    if (notches < 1) notches = 1;
    int pixels_per_notch = AppConfig::instance().scroll_pixels_per_notch;
    int scroll_amount = notches * pixels_per_notch;
    if (scroll_amount > AppConfig::instance().scroll_max) scroll_amount = AppConfig::instance().scroll_max;

    int from_y = static_cast<int>(y);
    int to_y;
    if (delta > 0) {
        to_y = from_y + scroll_amount;
    } else {
        to_y = from_y - scroll_amount;
    }
    if (to_y < 0) to_y = 0;

    int steps = (std::max)(8, scroll_amount / 30);

    if (use_socket_mode_) {
        std::lock_guard<std::mutex> lock(task_mutex_);
        task_queue_.push([this, x, from_y, to_y, steps]() {
            inject_touch_down(x, static_cast<uint32_t>(from_y));
            for (int i = 1; i <= steps; i++) {
                uint32_t cy = static_cast<uint32_t>(
                    from_y + (to_y - from_y) * i / steps);
                inject_touch_move(x, cy);
                Sleep(10);
            }
            inject_touch_up(x, static_cast<uint32_t>(to_y));
        });
        task_cv_.notify_one();
        return true;
    }
    int velocity = 800;
    return inject_swipe(x, static_cast<uint32_t>(from_y),
                        x, static_cast<uint32_t>(to_y), velocity);
}

void InputInjector::reconnect_loop() {
    LOGI("Reconnect loop started");
    int consecutive_failures = 0;
    while (reconnect_running_) {
        Sleep(2000);
        if (!reconnect_running_) break;

        // Drain agent responses to prevent its TCP send buffer from filling up.
        // The agent sends one response per request (raw JSON, no length prefix).
        // If we never drain, the agent eventually blocks and stops processing.
        int sock = control_sock_.load();
        if (sock >= 0) {
            auto sock_fd = static_cast<socket_t>(sock);
            fd_set rfds;
            struct timeval tv = {0, 0};  // non-blocking select
            FD_ZERO(&rfds);
            FD_SET(sock_fd, &rfds);
            while (select(0, &rfds, nullptr, nullptr, &tv) > 0) {
                char buf[512];
                int n = ::recv(sock_fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                FD_ZERO(&rfds);
                FD_SET(sock_fd, &rfds);
            }
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
        task();
    }
}

} // namespace hscrcpy
