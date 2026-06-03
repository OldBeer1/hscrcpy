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

InputInjector::InputInjector(HdcWrapper &hdc) : hdc_(hdc) {}

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
    use_abstract_socket_ = false;
    while (std::getline(s1, p1, '.') && std::getline(s2, p2, '.')) {
        int n1 = std::atoi(p1.c_str());
        int n2 = std::atoi(p2.c_str());
        if (n2 < n1) { use_abstract_socket_ = false; break; }
        if (n2 > n1) { use_abstract_socket_ = true; break; }
    }

    if (!start_uitest_daemon()) {
        LOGW("Failed to start uitest daemon");
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
    LOGI("Input injector initialized (socket mode, abstract=%s)", use_abstract_socket_ ? "true" : "false");
    return true;
}

void InputInjector::cleanup() {
    if (control_sock_ >= 0) {
        CLOSE_SOCKET(static_cast<socket_t>(control_sock_));
        control_sock_ = -1;
    }
    if (local_control_port_ > 0) {
        hdc_.forward_remove(local_control_port_, use_abstract_socket_,
                            use_abstract_socket_ ? kUitestSocketName : "");
    }
}

bool InputInjector::start_uitest_daemon() {
    std::string output;
    hdc_.shell_command("\"ps -ef | grep singleness\"", &output, 5);
    bool has_singleness = false;
    for (auto &line : output) {
        if (output.find("singleness") != std::string::npos &&
            output.find("extension-name") == std::string::npos &&
            output.find("grep") == std::string::npos) {
            has_singleness = true;
            break;
        }
    }
    if (has_singleness) {
        LOGI("uitest daemon already running (without extension)");
        return true;
    }
    hdc_.shell_command("uitest start-daemon singleness", nullptr, 5);
    Sleep(1000);
    return true;
}

bool InputInjector::push_agent_so() {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string exe_dir(exe_path);
    auto last_sep = exe_dir.find_last_of("\\/");
    if (last_sep != std::string::npos) exe_dir = exe_dir.substr(0, last_sep + 1);
    std::string server_dir = exe_dir + "server";

    std::string file_output;
    hdc_.shell_command("file /system/bin/uitest", &file_output, 5);
    bool is_x86 = file_output.find("x86_64") != std::string::npos;

    std::string agent_path;
    if (is_x86) {
        agent_path = server_dir + "\\uitest_agent_x86_1.1.9.so";
    } else {
        const char *agents[] = {
            "uitest_agent_1.2.2.so",
            "uitest_agent_1.1.10.so",
            "uitest_agent_1.1.5.so",
            "uitest_agent_1.1.3.so",
        };
        for (auto &name : agents) {
            std::string path = server_dir + "\\" + name;
            if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                agent_path = path;
                break;
            }
        }
    }

    if (agent_path.empty() || GetFileAttributesA(agent_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOGW("No agent SO found");
        return false;
    }

    std::string check_output;
    hdc_.shell_command("cat /data/local/tmp/agent.so | grep -a UITEST_AGENT_LIBRARY", &check_output, 5);
    if (check_output.find("UITEST_AGENT_LIBRARY") != std::string::npos) {
        LOGI("Agent already exists on device");
        return true;
    }

    return hdc_.push_file(agent_path, kDeviceAgentPath);
}

bool InputInjector::setup_control_port() {
    for (uint16_t port = 9950; port < 10050; port++) {
        bool ok;
        if (use_abstract_socket_) {
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
        LOGW("Failed to connect to control port %u", local_control_port_);
        CLOSE_SOCKET(static_cast<socket_t>(control_sock_));
        control_sock_ = -1;
        return false;
    }

    LOGI("Control socket connected on port %u", local_control_port_);
    return true;
}

bool InputInjector::send_to_socket(const std::string &json) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    if (control_sock_ < 0) {
        if (!setup_control_port()) return false;
    }

    LOGI("Sending to socket: %s", json.c_str());
    int n = ::send(static_cast<socket_t>(control_sock_),
                   json.c_str(), static_cast<int>(json.size()), 0);
    if (n <= 0) {
        LOGW("send to socket failed (n=%d), reconnecting...", n);
        CLOSE_SOCKET(static_cast<socket_t>(control_sock_));
        control_sock_ = -1;
        Sleep(100);
        if (setup_control_port()) {
            n = ::send(static_cast<socket_t>(control_sock_),
                       json.c_str(), static_cast<int>(json.size()), 0);
        }
    }
    return n > 0;
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
    return true;
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
