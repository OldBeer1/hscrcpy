#include "input_injector.h"
#include "log.h"
#include "protocol.h"
#include <cstdio>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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

InputInjector::InputInjector(HdcWrapper &hdc, uint16_t base_port) : hdc_(hdc), base_port_(base_port) {}

InputInjector::~InputInjector() { cleanup(); }

bool InputInjector::init(const std::string &uitest_version) {
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
        return true;
    }

    if (!setup_control_port()) {
        LOGW("Failed to setup control port, will use uitest uiInput shell commands");
        use_socket_mode_ = false;
        return true;
    }

    use_socket_mode_ = true;
    LOGI("Input injector initialized (socket mode, abstract_control=%s)", use_abstract_control_ ? "true" : "false");
    return true;
}

void InputInjector::cleanup() {
    if (control_sock_ >= 0) {
        CLOSE_SOCKET(static_cast<socket_t>(control_sock_));
        control_sock_ = -1;
    }
    if (local_control_port_ > 0) {
        hdc_.forward_remove(local_control_port_, use_abstract_control_,
                            use_abstract_control_ ? kUitestSocketName : "");
    }
}

bool InputInjector::ensure_uitest_agent() {
    for (int i = 0; i < 10; i++) {
        std::string output;

        hdc_.shell_command("cat /proc/net/unix | grep uitest_socket", &output, 3);
        if (output.find("uitest_socket") != std::string::npos) {
            LOGI("uitest agent listening on abstract socket uitest_socket (check %d)", i + 1);
            use_abstract_control_ = true;
            return true;
        }

        hdc_.shell_command("netstat -an | grep 8012 | grep LISTEN", &output, 3);
        if (output.find("8012") != std::string::npos && output.find("LISTEN") != std::string::npos) {
            LOGI("uitest agent listening on TCP port 8012 (check %d)", i + 1);
            use_abstract_control_ = false;
            return true;
        }

        if (i == 0) {
            LOGI("uitest agent not found yet, starting uitest daemon...");
            hdc_.shell_command("uitest start-daemon singleness", nullptr, 5);
        }
        Sleep(300);
    }

    LOGW("uitest agent still not available after checks");
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
    uint16_t start_port = base_port_ > 0 ? base_port_ + 1 : 9950;
    uint16_t end_port = 10050;

    for (uint16_t port = start_port; port < end_port; port++) {
        bool ok;
        if (use_abstract_control_) {
            ok = hdc_.forward_localabstract(port, kUitestSocketName);
        } else {
            ok = hdc_.forward_tcp(port, kDefaultControlPort);
        }
        if (ok) {
            local_control_port_ = port;
            break;
        }
    }

    if (local_control_port_ == 0) {
        LOGE("Failed to setup control port forward");
        return false;
    }

    control_sock_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (control_sock_ == static_cast<int>(INVALID_SOCK)) {
        LOGE("Failed to create control socket");
        return false;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(local_control_port_);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(static_cast<socket_t>(control_sock_),
                  reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        LOGW("Failed to connect to control port %u (abstract=%s)", local_control_port_,
             use_abstract_control_ ? "true" : "false");
        CLOSE_SOCKET(static_cast<socket_t>(control_sock_));
        control_sock_ = -1;
        return false;
    }

    LOGI("Control socket connected on port %u (abstract=%s)", local_control_port_,
         use_abstract_control_ ? "true" : "false");
    return true;
}

bool InputInjector::send_to_socket(const std::string &json) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    if (control_sock_ < 0) {
        if (!setup_control_port()) {
            LOGW("setup_control_port failed, falling back to shell mode");
            use_socket_mode_ = false;
            return false;
        }
    }

    int n = ::send(static_cast<socket_t>(control_sock_),
                   json.c_str(), static_cast<int>(json.size()), 0);
    if (n <= 0) {
        LOGW("send to socket failed (n=%d), reconnecting...", n);
        CLOSE_SOCKET(static_cast<socket_t>(control_sock_));
        control_sock_ = -1;
        Sleep(50);
        if (setup_control_port()) {
            n = ::send(static_cast<socket_t>(control_sock_),
                       json.c_str(), static_cast<int>(json.size()), 0);
            if (n <= 0) {
                LOGW("send still failed after reconnect");
                return false;
            }
        } else {
            LOGW("reconnect failed");
            return false;
        }
    }
    return true;
}

bool InputInjector::inject_click(uint32_t x, uint32_t y) {
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
        return send_to_socket(json);
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "uitest uiInput click %u %u", x, y);
    return hdc_.shell_command(cmd, nullptr, 5);
}

bool InputInjector::inject_long_click(uint32_t x, uint32_t y) {
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
        return send_to_socket(json);
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "uitest uiInput longClick %u %u", x, y);
    return hdc_.shell_command(cmd, nullptr, 5);
}

bool InputInjector::inject_touch_down(uint32_t x, uint32_t y) {
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"touchDown\",\"args\":{\"x\":%u,\"y\":%u}}}",
                 x, y);
        return send_to_socket(json);
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "uitest uiInput click %u %u", x, y);
    return hdc_.shell_command(cmd, nullptr, 5);
}

bool InputInjector::inject_touch_move(uint32_t x, uint32_t y) {
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"touchMove\",\"args\":{\"x\":%u,\"y\":%u}}}",
                 x, y);
        return send_to_socket(json);
    }
    return true;
}

bool InputInjector::inject_touch_up(uint32_t x, uint32_t y) {
    if (use_socket_mode_) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"module\":\"com.ohos.devicetest.hypiumApiHelper\",\"method\":\"Gestures\","
                 "\"params\":{\"api\":\"touchUp\",\"args\":{\"x\":%u,\"y\":%u}}}",
                 x, y);
        return send_to_socket(json);
    }
    return true;
}

bool InputInjector::inject_swipe(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, uint32_t velocity) {
    if (use_socket_mode_) {
        inject_touch_down(from_x, from_y);
        Sleep(20);
        int steps = 10;
        for (int i = 1; i <= steps; i++) {
            uint32_t cx = from_x + (to_x - from_x) * i / steps;
            uint32_t cy = from_y + (to_y - from_y) * i / steps;
            inject_touch_move(cx, cy);
            Sleep(velocity / steps / 2);
        }
        return inject_touch_up(to_x, to_y);
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "uitest uiInput swipe %u %u %u %u %u", from_x, from_y, to_x, to_y, velocity);
    return hdc_.shell_command(cmd, nullptr, 5);
}

bool InputInjector::inject_back() {
    return hdc_.shell_command("uinput -K -d 2 -u 2", nullptr, 3);
}

bool InputInjector::inject_home() {
    return hdc_.shell_command("uinput -K -d 1 -u 1", nullptr, 3);
}

bool InputInjector::inject_power() {
    return hdc_.shell_command("uinput -K -d 18 -u 18", nullptr, 3);
}

bool InputInjector::inject_key_event(const std::string &key) {
    return hdc_.shell_command("uinput -K -d " + key + " -u " + key, nullptr, 3);
}

bool InputInjector::inject_reboot() {
    return hdc_.shell_command("reboot", nullptr, 5);
}

bool InputInjector::inject_wheel(uint32_t x, uint32_t y, int32_t delta) {
    uint32_t distance = 200;
    uint32_t velocity = 600;
    if (delta > 0) {
        return inject_swipe(x, y + distance, x, y - distance, velocity);
    } else {
        return inject_swipe(x, y - distance, x, y + distance, velocity);
    }
}

} // namespace hscrcpy
