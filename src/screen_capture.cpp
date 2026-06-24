#include "screen_capture.h"
#include "config.h"
#include "log.h"
#include "protocol.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <sstream>

namespace hscrcpy {

ScreenCapture::ScreenCapture(HdcWrapper &hdc, const CaptureConfig &config)
    : hdc_(hdc), config_(config) {
    auto &ac = AppConfig::instance();
    max_h264_queue_size_ = ac.max_h264_queue;
    max_grpc_reconnect_ = ac.max_grpc_reconnect;
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string exe_dir(exe_path);
    auto last_sep = exe_dir.find_last_of("\\/");
    if (last_sep != std::string::npos) exe_dir = exe_dir.substr(0, last_sep + 1);
    server_dir_ = exe_dir + "server";
}

ScreenCapture::~ScreenCapture() {
    stop();
}

static std::string version_compare_result(const std::string &v1, const std::string &v2) {
    std::istringstream s1(v1), s2(v2);
    std::string p1, p2;
    while (std::getline(s1, p1, '.') && std::getline(s2, p2, '.')) {
        int n1 = std::atoi(p1.c_str());
        int n2 = std::atoi(p2.c_str());
        if (n1 < n2) return "less";
        if (n1 > n2) return "greater";
    }
    if (std::getline(s1, p1, '.')) return "greater";
    if (std::getline(s2, p2, '.')) return "less";
    return "equal";
}

std::string ScreenCapture::get_local_so_md5(const std::string &filepath) {
    std::string output;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "CertUtil -hashfile \"%s\" MD5", filepath.c_str());

    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE hRead, hWrite;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::string full_cmd = "cmd.exe /C CertUtil -hashfile \"" + filepath + "\" MD5";
    if (!CreateProcessA(NULL, const_cast<char *>(full_cmd.c_str()),
                        NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return "";
    }

    CloseHandle(hWrite);
    char buf[8192];
    DWORD bytesRead;
    while (ReadFile(hRead, buf, sizeof(buf), &bytesRead, NULL) && bytesRead > 0) {
        output.append(buf, bytesRead);
    }
    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::string md5;
    std::string line;
    for (auto &c : output) {
        if (c == '\n' || c == '\r') {
            std::string clean;
            for (auto &ch : line) {
                if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) {
                    clean += static_cast<char>(tolower(ch));
                }
            }
            if (clean.size() == 32) {
                md5 = clean;
                break;
            }
            line.clear();
        } else {
            line += c;
        }
    }
    if (md5.empty()) {
        std::string clean;
        for (auto &ch : line) {
            if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) {
                clean += static_cast<char>(tolower(ch));
            }
        }
        if (clean.size() == 32) md5 = clean;
    }
    return md5;
}

std::string ScreenCapture::find_matching_so_with_md5(const std::string &device_md5) {
    // HOScrcpy: isUseSecSo = compareUitestVersion("6.0.2.1", uitestVersion) < 0
    // i.e. uitest >= 6.0.2.1 → sec SO (unix_6.*) → abstract socket
    //      uitest < 6.0.2.1 → normal SO (server0-3, 5.10) → TCP port 5000
    // We follow the same logic, with cross-list fallback in case the primary
    // list's SOs are all incompatible (e.g. sec SOs fail on some uitest 5.x).
    bool prefer_sec = version_compare_result(uitest_version_, "6.0.2.1") != "less";

    // Primary list: matches HOScrcpy's version-based selection
    // Fallback list: the other category, tried if primary list is empty
    std::vector<std::string> primary = prefer_sec
        ? std::vector<std::string>{
            "libscrcpy_server_unix_6.6-20260418.z.so",
            "libscrcpy_server_unix_6.5-20260313.z.so",
            "libscrcpy_server_unix_6.4-20260113.z.so",
            "libscrcpy_server_unix_6.3.1-20260113.z.so",
        }
        : std::vector<std::string>{
            "libscrcpy_server1.z.so",
            "libscrcpy_server2.z.so",
            "libscrcpy_server3.z.so",
            "libscrcpy_server_5.10-20260114.z.so",
        };

    std::vector<std::string> fallback = prefer_sec
        ? std::vector<std::string>{
            "libscrcpy_server1.z.so",
            "libscrcpy_server0.z.so",
            "libscrcpy_server2.z.so",
            "libscrcpy_server3.z.so",
        }
        : std::vector<std::string>{
            "libscrcpy_server_unix_6.6-20260418.z.so",
            "libscrcpy_server_unix_6.5-20260313.z.so",
            "libscrcpy_server_unix_6.4-20260113.z.so",
            "libscrcpy_server_unix_6.3.1-20260113.z.so",
        };

    // First pass: MD5 match (device already has a working SO from a previous session).
    // HOScrcpy also checks MD5 first and overrides isUseSecSo based on which map matches.
    // Check primary list first, then fallback list.
    for (auto &list : {primary, fallback}) {
        for (auto &name : list) {
            std::string path = server_dir_ + "\\" + name;
            if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                std::string local_md5 = get_local_so_md5(path);
                if (!device_md5.empty() && local_md5 == device_md5) {
                    LOGI("Selected server SO: %s (MD5 match, skip push)", name.c_str());
                    return path;
                }
            }
        }
    }

    // Second pass: pick first available from primary list, then fallback list
    for (auto &list : {primary, fallback}) {
        for (auto &name : list) {
            std::string path = server_dir_ + "\\" + name;
            if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                LOGI("Selected server SO: %s (by priority, no MD5 match)", name.c_str());
                return path;
            }
        }
    }

    // Last resort: find any .z.so file in server directory
    WIN32_FIND_DATAA fd;
    std::string pattern = server_dir_ + "\\*.z.so";
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        std::string result;
        do {
            if (strstr(fd.cFileName, "emulator") == nullptr) {
                result = server_dir_ + "\\" + fd.cFileName;
                break;
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
        if (!result.empty()) return result;
    }

    return "";
}

std::string ScreenCapture::find_matching_agent_so() {
    std::string file_output;
    hdc_.shell_command("file /system/bin/uitest", &file_output, 5);
    bool is_x86 = file_output.find("x86_64") != std::string::npos ||
                  file_output.find("x86") != std::string::npos;

    if (is_x86) {
        std::string path = server_dir_ + "\\uitest_agent_x86_1.1.9.so";
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }

    if (version_compare_result(uitest_version_, "5.1.1.2") != "less") {
        std::string path = server_dir_ + "\\uitest_agent_1.1.3.so";
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    if (version_compare_result(uitest_version_, "5.0.1.6") != "less") {
        std::string path = server_dir_ + "\\uitest_agent_1.1.5.so";
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    if (version_compare_result(uitest_version_, "6.0.0.0") != "less") {
        std::string path = server_dir_ + "\\uitest_agent_1.1.10.so";
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }

    const char *agents[] = {
        "uitest_agent_1.2.2.so",
        "uitest_agent_1.1.10.so",
        "uitest_agent_1.1.5.so",
        "uitest_agent_1.1.3.so",
        "uitest_agent_x86_1.1.9.so",
    };
    for (auto &name : agents) {
        std::string path = server_dir_ + "\\" + name;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return "";
}

std::string ScreenCapture::build_extension_cmd() const {
    char cmd[512];
    // HOScrcpy behavior: always use "-p 5000" only. The sec SO (for uitest < 6.0.2.1)
    // automatically creates the scrcpy_grpc_socket abstract socket; no need to pass it
    // as a command-line argument. Passing it explicitly can cause the extension to fail
    // to create the abstract socket on some 5.x devices.
    // Trailing '&' runs uitest in background on the device, matching HOScrcpy behavior.
    snprintf(cmd, sizeof(cmd),
             "/system/bin/uitest start-daemon singleness --extension-name libscreen_casting.z.so "
             "-scale %d -frameRate %d -bitRate %d -p 5000 -screenId 0 -encodeType 0 "
             "-iFrameInterval %d -repeatInterval %d &",
             config_.scale, config_.frame_rate, config_.bit_rate,
             config_.iframe_interval,
             1000 / config_.frame_rate);
    return cmd;
}

bool ScreenCapture::start_uitest_with_extension(bool force_restart) {
    hdc_.ensure_connected();

    for (int precheck = 0; precheck < 3; precheck++) {
        std::string check;
        hdc_.shell_command("echo ok", &check, 3);
        if (check.find("ok") != std::string::npos) break;
        LOGW("hdc not ready (attempt %d/3): '%s'", precheck + 1, check.c_str());
        Sleep(2000);
    }

    std::string ver_raw;
    {
        for (int attempt = 0; attempt < 3; attempt++) {
            std::string ver_output;
            hdc_.shell_command("/system/bin/uitest --version", &ver_output, 8);
            if (ver_output.find("[Fail]") != std::string::npos || ver_output.find("Fail") != std::string::npos) {
                LOGW("uitest --version returned error (attempt %d): '%s'", attempt + 1, ver_output.c_str());
                Sleep(1000);
                continue;
            }
            std::string ver_line;
            for (auto &c : ver_output) {
                if (c == '\r' || c == '\n') {
                    if (!ver_line.empty()) break;
                    continue;
                }
                ver_line += c;
            }
            for (auto &c : ver_line) {
                if ((c >= '0' && c <= '9') || c == '.') ver_raw += c;
                else if (!ver_raw.empty()) break;
            }
            if (!ver_raw.empty() && ver_raw.find('.') != std::string::npos) {
                uitest_version_ = ver_raw;
                break;
            } else if (!ver_raw.empty()) {
                LOGW("Parsed uitest version '%s' has no dot, ignoring", ver_raw.c_str());
            }
            break;
        }
    }

    std::string device_md5;
    {
        for (int attempt = 0; attempt < 3; attempt++) {
            std::string md5_output;
            char md5_cmd[256];
            snprintf(md5_cmd, sizeof(md5_cmd), "md5sum %s", kDeviceSoPath);
            hdc_.shell_command(md5_cmd, &md5_output, 8);
            if (md5_output.find("[Fail]") != std::string::npos || md5_output.find("Fail") != std::string::npos) {
                LOGW("md5sum returned error (attempt %d): '%s'", attempt + 1, md5_output.c_str());
                Sleep(1000);
                continue;
            }
            auto pos = md5_output.find(' ');
            if (pos == std::string::npos) pos = md5_output.find('\t');
            if (pos != std::string::npos) device_md5 = md5_output.substr(0, pos);
            while (!device_md5.empty() && (device_md5.back() == '\r' || device_md5.back() == '\n'))
                device_md5.pop_back();
            break;
        }
    }

    std::string ps_output;
    {
        std::string ps_raw;
        hdc_.shell_command("ps -ef", &ps_raw, 5);
        std::istringstream iss(ps_raw);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("extension-name") != std::string::npos && line.find("grep") == std::string::npos) {
                ps_output += line + "\n";
            }
        }
    }

    LOGI("uitest version: '%s', device_md5: '%s'", uitest_version_.c_str(), device_md5.c_str());

    // use_abstract_socket_ will be set after SO selection based on which SO is deployed.
    // HOScrcpy determines socket mode by the actual SO type, not just uitest version:
    // sec SO (unix_6.*) → abstract socket, normal SO (server0-3, 5.10) → TCP port 5000.

    bool extension_running = false;
    std::vector<std::string> pids;
    std::istringstream piss(ps_output);
    std::string line;
    while (std::getline(piss, line)) {
        if (line.find("extension-name") == std::string::npos) continue;
        if (line.find("grep") != std::string::npos) continue;
        extension_running = true;
        std::istringstream ls(line);
        std::string user, pid_str;
        if (ls >> user >> pid_str) {
            bool all_digit = true;
            for (char c : pid_str) { if (c < '0' || c > '9') { all_digit = false; break; } }
            if (all_digit && !pid_str.empty()) pids.push_back(pid_str);
        }
    }

    std::string local_so = find_matching_so_with_md5(device_md5);
    if (local_so.empty()) {
        LOGE("No matching server .z.so found in %s", server_dir_.c_str());
        return false;
    }
    LOGI("Using server SO: %s", local_so.c_str());

    // HOScrcpy overrides isUseSecSo based on which SO is actually deployed:
    // sec SO (unix_6.*) creates abstract socket, normal SO (server0-3, 5.10) uses TCP.
    // When MD5 matches a normal SO on a uitest < 6.0.2.1 device, HOScrcpy switches
    // to TCP mode — the SO variant determines the socket mode, not just uitest version.
    bool is_sec_so = local_so.find("unix_6") != std::string::npos;
    use_abstract_socket_ = is_sec_so;

    // Fallback: if uitest version is unknown but the device already has an abstract
    // socket from a previous session, prefer abstract socket mode. This handles the
    // case where the local server directory lacks the sec SO file (MD5 match failed)
    // but the device is already running with sec SO.
    if (!use_abstract_socket_ && uitest_version_.empty() && !extension_running) {
        std::string check;
        hdc_.shell_command("cat /proc/net/unix", &check, 3);
        if (check.find(kGrpcSocketName) != std::string::npos) {
            use_abstract_socket_ = true;
            LOGI("uitest version unknown but abstract socket found, using abstract socket mode");
        }
    }

    LOGI("use_abstract_socket: %s (based on SO type: %s)", use_abstract_socket_ ? "true" : "false",
         is_sec_so ? "sec SO (abstract socket)" : "normal SO (TCP)");

    std::string local_md5 = get_local_so_md5(local_so);
    bool so_up_to_date = !device_md5.empty() && (local_md5 == device_md5);
    LOGI("Device SO md5: %s, Local SO md5: %s, up_to_date: %s",
         device_md5.c_str(), local_md5.c_str(), so_up_to_date ? "true" : "false");

    // Always kill and restart the extension on a new connection.
    // Reusing a stale extension leads to gRPC connection succeeding but
    // receiving no frames (the extension accepts TCP but doesn't stream).
    // The uitest daemon itself is never killed — only the extension process.
    if (extension_running && !pids.empty()) {
        LOGI("Killing existing uitest extension (%zu process(es))", pids.size());
        std::string kill_cmd = "kill -9";
        for (auto &p : pids) kill_cmd += " " + p;
        hdc_.shell_command(kill_cmd, nullptr, 3);
        Sleep(1000);
    } else if (extension_running) {
        LOGI("Extension running but PID not found, killing by name");
        hdc_.shell_command("pkill -9 -f extension-name", nullptr, 3);
        Sleep(1000);
    }

    std::string daemon_check;
    hdc_.shell_command("ps -ef", &daemon_check, 3);
    bool daemon_running = false;
    {
        std::istringstream iss(daemon_check);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("uitest") != std::string::npos &&
                line.find("start-daemon") != std::string::npos &&
                line.find("grep") == std::string::npos &&
                line.find("extension-name") == std::string::npos) {
                daemon_running = true;
                break;
            }
        }
    }
    if (!daemon_running) {
        LOGI("uitest main daemon not running, starting...");
        hdc_.shell_command("uitest start-daemon singleness", nullptr, 5);
        Sleep(500);
    } else {
        LOGI("uitest main daemon already running");
    }

    LOGI("Pushing %s to %s (md5_match=%s)", local_so.c_str(), kDeviceSoPath, so_up_to_date ? "yes" : "no");
    if (!so_up_to_date) {
        if (!hdc_.push_file(local_so, kDeviceSoPath)) {
            LOGE("Failed to push server SO");
            return false;
        }
    } else {
        LOGI("SO already up to date, skipping push");
    }

    std::string cmd = build_extension_cmd();
    LOGI("Starting uitest extension: %s", cmd.c_str());
    hdc_.shell_command(cmd, nullptr, 5);

    if (!wait_for_extension_ready()) {
        return false;
    }

    Sleep(1000);

    uitest_port_ = kDefaultVideoPort;
    return true;
}

bool ScreenCapture::wait_for_extension_ready() {
    bool extension_ready = false;
    for (int i = 0; i < 30; i++) {
        Sleep(100);
        std::string check;
        if (use_abstract_socket_) {
            hdc_.shell_command("cat /proc/net/unix", &check, 3);
            if (check.find(kGrpcSocketName) != std::string::npos) {
                LOGI("uitest extension ready (abstract socket) after %d ms", (i + 1) * 100);
                extension_ready = true;
                break;
            }
        } else {
            // Check TCP port 5000 — HarmonyOS netstat may not show it reliably,
            // so we also check the extension process as a fallback indicator.
            hdc_.shell_command("netstat -an", &check, 3);
            bool port_listening = false;
            std::istringstream nss(check);
            std::string nline;
            while (std::getline(nss, nline)) {
                if (nline.find("LISTEN") == std::string::npos) continue;
                if (nline.find(":5000") != std::string::npos) {
                    port_listening = true;
                    break;
                }
            }
            if (port_listening) {
                LOGI("uitest extension ready (port 5000) after %d ms", (i + 1) * 100);
                extension_ready = true;
                break;
            }
            // Fallback: check if extension process is running and abstract socket exists
            hdc_.shell_command("cat /proc/net/unix", &check, 3);
            if (check.find(kGrpcSocketName) != std::string::npos) {
                LOGI("uitest extension ready (abstract socket fallback) after %d ms", (i + 1) * 100);
                use_abstract_socket_ = true;
                extension_ready = true;
                break;
            }
            // Final fallback: if extension process exists after a reasonable wait,
            // assume it's ready (netstat may be unreliable on some HarmonyOS versions)
            if (i >= 5) {
                std::string ps_check;
                hdc_.shell_command("ps -ef", &ps_check, 3);
                std::istringstream pss(ps_check);
                bool ext_found = false;
                while (std::getline(pss, nline)) {
                    if (nline.find("extension-name") != std::string::npos &&
                        nline.find("grep") == std::string::npos) {
                        ext_found = true;
                        break;
                    }
                }
                if (ext_found) {
                    LOGI("uitest extension ready (process detected, netstat unreliable) after %d ms", (i + 1) * 100);
                    extension_ready = true;
                    break;
                }
            }
        }
    }

    if (!extension_ready) {
        LOGE("uitest extension failed to start (%s not listening after 3s)",
             use_abstract_socket_ ? "abstract socket" : "port 5000");

        std::string ps_debug;
        hdc_.shell_command("ps -ef", &ps_debug, 3);
        {
            std::istringstream iss(ps_debug);
            std::string line;
            std::string filtered;
            while (std::getline(iss, line)) {
                if (line.find("uitest") != std::string::npos && line.find("grep") == std::string::npos) {
                    filtered += line + "\n";
                }
            }
            LOGI("uitest processes after failed start: '%s'", filtered.c_str());
        }
        return false;
    }

    return true;
}

static bool is_local_port_free(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool free = (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    closesocket(s);
    return free;
}

bool ScreenCapture::setup_port_forward() {
    hdc_.cleanup_stale_forwards();

    for (uint16_t base = AppConfig::instance().local_base_port; base < AppConfig::instance().local_base_port + AppConfig::instance().local_port_range; base += 2) {
        if (!is_local_port_free(base) || !is_local_port_free(base + 1)) continue;

        local_video_port_ = base;
        local_control_port_ = base + 1;

        bool ok;
        if (use_abstract_socket_) {
            ok = hdc_.forward_localabstract(local_video_port_, kGrpcSocketName);
        } else {
            ok = hdc_.forward_tcp(local_video_port_, uitest_port_);
        }
        if (ok) {
            local_base_port_ = base;
            char remote_info[64];
            if (use_abstract_socket_) {
                snprintf(remote_info, sizeof(remote_info), "%s", kGrpcSocketName);
            } else {
                snprintf(remote_info, sizeof(remote_info), "tcp:%u", uitest_port_);
            }
            LOGI("Port forward: local %u -> %s (control will use %u)",
                 local_video_port_, remote_info, local_control_port_);
            break;
        }
        local_video_port_ = 0;
        local_control_port_ = 0;
    }

    if (local_video_port_ == 0) {
        LOGE("Failed to set up port forward (no free local port)");
        return false;
    }
    return true;
}

bool ScreenCapture::setup_grpc_stream(bool force_restart) {
    LOGI("setup_grpc_stream: serial='%s'%s", hdc_.serial().c_str(), force_restart ? " (force_restart)" : "");

    if (!decoder_.init()) {
        LOGW("H264 decoder not available (FFmpeg DLLs not found), skipping gRPC stream");
        LOGW("To enable gRPC virtual screen streaming, download FFmpeg shared DLLs from:");
        LOGW("https://github.com/BtbN/FFmpeg-Builds/releases");
        LOGW("Copy avcodec-62.dll, avutil-60.dll, swscale-9.dll, swresample-6.dll to the exe directory");
        return false;
    }

    if (!start_uitest_with_extension(force_restart)) {
        LOGE("Failed to start uitest with extension");
        return false;
    }

    if (!setup_port_forward()) {
        LOGE("Failed to setup port forward");
        return false;
    }

    bool connected = false;
    grpc_client_.set_idle_timeout_sec(AppConfig::instance().idle_timeout_sec);
    for (int retry = 0; retry < 5; retry++) {
        LOGI("gRPC connect attempt %d to 127.0.0.1:%u", retry + 1, local_video_port_);
        if (grpc_client_.connect("127.0.0.1", local_video_port_)) {
            connected = true;
            LOGI("gRPC connected successfully");
            break;
        }
        if (retry == 1) {
            LOGI("Retrying uitest extension start...");
            hdc_.shell_command(build_extension_cmd(), nullptr, 5);
            Sleep(1000);
        } else {
            Sleep(500);
        }
    }

    if (!connected) {
        LOGE("Failed to connect gRPC server after retries");
        return false;
    }

    mode_ = CaptureMode::kGrpcStream;
    LOGI("gRPC stream setup complete, starting streaming");

    return true;
}

bool ScreenCapture::setup_snapshot_fallback() {
    mode_ = CaptureMode::kSnapshotFallback;
    LOGI("Using snapshot_display fallback mode");

    hdc_.shell_command("uitest start-daemon singleness", nullptr, 5);
    Sleep(1000);

    std::string output;
    hdc_.shell_command("snapshot_display -f /data/local/tmp/screen.jpeg", &output, 10);
    unsigned w = 0, h = 0;
    if (sscanf(output.c_str(), "process: display %*d: width %u, height %u", &w, &h) == 2 ||
        sscanf(output.c_str(), " %*[^w]width %u, height %u", &w, &h) == 2) {
        width_ = w;
        height_ = h;
        LOGI("Screen resolution from snapshot: %ux%u", width_.load(), height_.load());
    }

    if (width_ == 0) {
        width_ = 1260;
        height_ = 2720;
        LOGI("Using default resolution: %ux%u", width_.load(), height_.load());
    }

    return true;
}

bool ScreenCapture::start() {
    if (running_) return true;

    LOGI("Screen capture starting");

    bool grpc_ok = false;
    try {
        grpc_ok = setup_grpc_stream();
    } catch (const std::exception &e) {
        LOGE("gRPC setup exception: %s", e.what());
        grpc_ok = false;
    }

    if (!grpc_ok) {
        LOGW("gRPC stream failed, falling back to snapshot_display mode");
        grpc_client_.disconnect();
        if (local_video_port_ > 0) {
            hdc_.forward_remove(local_video_port_, use_abstract_socket_,
                                use_abstract_socket_ ? kGrpcSocketName : "");
            local_video_port_ = 0;
        }
        if (local_control_port_ > 0) {
            hdc_.forward_remove(local_control_port_, use_abstract_socket_,
                                use_abstract_socket_ ? kUitestSocketName : "");
            local_control_port_ = 0;
        }
        if (!setup_snapshot_fallback()) {
            LOGE("Snapshot fallback also failed");
            return false;
        }
    }

    if (mode_ == CaptureMode::kGrpcStream && grpc_ok) {
        running_ = true;
        got_stream_data_ = false;
        // Initialize frame pool (3-buffer) - lazy allocation:
        // actual memory is allocated on first decode via swap with decoder's image_
        for (auto &f : frame_pool_) {
            f = std::make_shared<DecodedImage>();
        }
        pool_idx_ = 0;
        if (!frame_event_) frame_event_ = CreateEventA(NULL, FALSE, FALSE, NULL);
        decode_thread_ = std::thread(&ScreenCapture::decode_loop, this);
        thread_ = std::thread(&ScreenCapture::grpc_stream_loop, this);

        uint64_t frames_before = frame_count_.load();

        for (int i = 0; i < 30 && frame_count_.load() == frames_before; i++) {
            if (i == 3 && frame_count_.load() == frames_before) {
                LOGI("No frame after 300ms, requesting IDR frame...");
                grpc_client_.request_idr_frame();
            }
            if (i == 8 && frame_count_.load() == frames_before) {
                LOGI("No frame after 800ms, sending screen wakeup trigger...");
                hdc_.shell_command("power-shell wakeup", nullptr, 5);
                Sleep(100);
                hdc_.shell_command("uinput -M -m 100 100 500 500 --trace", nullptr, 5);
            }
            if (i == 18 && frame_count_.load() == frames_before) {
                LOGI("No frame after 1800ms, retrying IDR + trigger...");
                grpc_client_.request_idr_frame();
                hdc_.shell_command("uinput -M -m 200 200 100 100 --trace", nullptr, 5);
            }
            if (frame_event_) WaitForSingleObject(frame_event_, 100);
            else Sleep(100);
        }

        if (frame_count_.load() > frames_before) {
            LOGI("First frame received, streaming OK");
        } else {
            LOGI("No frame yet after 3s, continuing (screen may be static)...");
        }
        return true;
    }

    running_ = true;
    // Initialize frame pool (3-buffer) - lazy allocation
    for (auto &f : frame_pool_) {
        f = std::make_shared<DecodedImage>();
    }
    pool_idx_ = 0;
    if (!frame_event_) frame_event_ = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (mode_ == CaptureMode::kGrpcStream) {
        thread_ = std::thread(&ScreenCapture::grpc_stream_loop, this);
    } else {
        thread_ = std::thread(&ScreenCapture::snapshot_loop, this);
    }
    return true;
}

void ScreenCapture::stop() {
    running_ = false;
    h264_queue_cv_.notify_all();
    if (decode_thread_.joinable()) decode_thread_.join();
    if (thread_.joinable()) thread_.join();

    grpc_client_.disconnect();
    decoder_.cleanup();

    if (frame_event_) { CloseHandle(frame_event_); frame_event_ = nullptr; }

    if (local_video_port_ > 0) {
        hdc_.forward_remove(local_video_port_, use_abstract_socket_,
                            use_abstract_socket_ ? kGrpcSocketName : "");
        local_video_port_ = 0;
    }

    if (local_control_port_ > 0) {
        hdc_.forward_remove(local_control_port_, use_abstract_socket_,
                            use_abstract_socket_ ? kUitestSocketName : "");
        local_control_port_ = 0;
    }

    hdc_.cleanup_stale_forwards({local_video_port_, local_control_port_});

    kill_uitest_extension();
}

void ScreenCapture::kill_uitest_extension() {
    std::string ps_raw;
    hdc_.shell_command("ps -ef", &ps_raw, 5);
    LOGI("kill_uitest_extension: ps output='%s'", ps_raw.c_str());
    std::istringstream iss(ps_raw);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("extension-name") == std::string::npos || line.find("grep") != std::string::npos) continue;
        std::istringstream ls(line);
        std::string user, pid_str;
        if (ls >> user >> pid_str) {
            bool all_digit = true;
            for (char c : pid_str) { if (c < '0' || c > '9') { all_digit = false; break; } }
            if (all_digit && !pid_str.empty()) {
                std::string kill_result;
                hdc_.shell_command("kill -9 " + pid_str, &kill_result, 3);
                LOGI("kill -9 %s result: '%s'", pid_str.c_str(), kill_result.c_str());
            }
        }
    }
    Sleep(300);
}

bool ScreenCapture::reconnect_grpc_stream() {
    LOGI("Attempting gRPC reconnection...");

    // Check if hdc connection to device is still alive before attempting reconnection.
    // If the device disconnected (e.g. TCP device went offline), all hdc shell commands
    // will time out and reconnection is impossible — we should fail fast.
    if (!hdc_.is_device_reachable()) {
        // For TCP devices, the device may have reconnected with a new port.
        // Try to find it in the device list and update the serial.
        if (hdc_.is_tcp() && hdc_.try_reconnect_new_port()) {
            LOGI("Device reconnected with new port, continuing reconnection");
        } else {
            LOGE("Reconnect: device is not reachable via hdc (TCP connection may be lost)");
            return false;
        }
    }

    // For TCP devices, re-establish the hdc connection
    if (hdc_.is_tcp()) {
        if (!hdc_.ensure_connected()) {
            LOGE("Reconnect: failed to re-establish hdc connection");
            return false;
        }
    }

    grpc_client_.disconnect();
    Sleep(500);

    // Save ports before clearing so cleanup can identify our forwards
    uint16_t old_video_port = local_video_port_;
    uint16_t old_control_port = local_control_port_;

    if (local_video_port_ > 0) {
        hdc_.forward_remove(local_video_port_, use_abstract_socket_,
                            use_abstract_socket_ ? kGrpcSocketName : "");
        local_video_port_ = 0;
    }
    if (local_control_port_ > 0) {
        hdc_.forward_remove(local_control_port_, use_abstract_socket_,
                            use_abstract_socket_ ? kUitestSocketName : "");
        local_control_port_ = 0;
    }

    hdc_.cleanup_stale_forwards({old_video_port, old_control_port});
    Sleep(300);

    // Kill and restart extension (same as fresh connection)
    kill_uitest_extension();
    hdc_.shell_command(build_extension_cmd(), nullptr, 5);

    if (!wait_for_extension_ready()) {
        LOGE("Reconnect: uitest extension failed to start");
        return false;
    }

    Sleep(1000);

    if (!setup_port_forward()) {
        LOGE("Reconnect: failed to setup port forward");
        return false;
    }

    bool connected = false;
    for (int retry = 0; retry < 3; retry++) {
        LOGI("Reconnect gRPC attempt %d to 127.0.0.1:%u", retry + 1, local_video_port_);
        if (grpc_client_.connect("127.0.0.1", local_video_port_)) {
            connected = true;
            break;
        }
        Sleep(500);
    }

    if (!connected) {
        LOGE("Reconnect: failed to connect gRPC server");
        return false;
    }

    LOGI("gRPC reconnected successfully");
    return true;
}

void ScreenCapture::grpc_stream_loop() {
    LOGI("gRPC stream loop started");

    auto on_frame = [this](const uint8_t *data, size_t size) {
        got_stream_data_ = true;
        {
            std::lock_guard<std::mutex> lock(h264_queue_mutex_);
            if (h264_queue_.size() >= max_h264_queue_size_) {
                h264_queue_.pop_front();
            }
            h264_queue_.push_back(std::make_shared<std::vector<uint8_t>>(data, data + size));
        }
        h264_queue_cv_.notify_one();
    };

    if (!grpc_client_.start_streaming(on_frame)) {
        LOGE("Failed to start gRPC streaming");
        running_ = false;
        return;
    }

    LOGI("gRPC streaming started (IDR already requested)");

    int reconnect_attempts = 0;
    uint64_t last_frame_count = frame_count_.load();
    int hdc_check_counter = 0;  // Check hdc reachability every ~10s while streaming

    while (running_) {
        while (running_ && grpc_client_.is_connected() && grpc_client_.is_streaming_active()) {
            Sleep(100);

            // If PING keepalive is failing, break out to trigger reconnection
            if (grpc_client_.is_ping_failing()) {
                LOGW("PING keepalive failing, breaking out of streaming loop");
                break;
            }

            // Periodically check hdc reachability for TCP devices (~every 10s)
            if (hdc_.is_tcp()) {
                hdc_check_counter++;
                if (hdc_check_counter >= 100) {
                    hdc_check_counter = 0;
                    if (!hdc_.is_device_reachable()) {
                        // Device may have reconnected with new port, try to find it
                        if (hdc_.try_reconnect_new_port()) {
                            LOGI("Device reconnected with new port during streaming, triggering gRPC reconnect");
                            break;  // Break inner loop to trigger gRPC reconnection
                        }
                        LOGE("Device disconnected (hdc unreachable during streaming)");
                        if (notify_hwnd_) {
                            PostMessageA(notify_hwnd_, kMsgDeviceDisconnected, 0, 0);
                        }
                        running_ = false;
                        break;
                    }
                }
            }
        }

        if (!running_) break;

        reconnecting_ = true;

        if (!grpc_client_.is_connected() || !grpc_client_.is_streaming_active()) {
            LOGW("gRPC stream disconnected (connected=%d, streaming=%d), attempting reconnect (%d/%d)",
                 grpc_client_.is_connected(), grpc_client_.is_streaming_active(),
                 reconnect_attempts + 1, max_grpc_reconnect_);
        }

        grpc_client_.stop_streaming();

        if (reconnect_attempts >= max_grpc_reconnect_) {
            LOGE("Max reconnect attempts reached, giving up");
            break;
        }

        reconnect_attempts++;

        if (!reconnect_grpc_stream()) {
            LOGE("Reconnect failed (attempt %d/%d)", reconnect_attempts, max_grpc_reconnect_);

            // If hdc connection to device is lost, try to find device with new port.
            // Only give up if the device is truly gone from the hdc device list.
            if (!hdc_.is_device_reachable()) {
                if (hdc_.is_tcp() && hdc_.try_reconnect_new_port()) {
                    LOGI("Device reconnected with new port, retrying gRPC reconnection");
                    continue;
                }
                LOGE("Device disconnected (hdc unreachable), stopping reconnection");
                if (notify_hwnd_) {
                    PostMessageA(notify_hwnd_, kMsgDeviceDisconnected, 0, 0);
                }
                break;
            }

            if (reconnect_attempts >= max_grpc_reconnect_) break;
            Sleep(1000);
            continue;
        }

        if (!grpc_client_.start_streaming(on_frame)) {
            LOGE("Failed to restart streaming after reconnect");
            Sleep(1000);
            continue;
        }

        LOGI("gRPC streaming restarted after reconnect (attempt %d)", reconnect_attempts);

        uint64_t frames_before = frame_count_.load();
        grpc_client_.request_idr_frame();

        for (int i = 0; i < 30 && running_ && frame_count_.load() == frames_before; i++) {
            Sleep(100);
        }

        if (frame_count_.load() > frames_before) {
            LOGI("Reconnect successful, frames flowing again");
            reconnect_attempts = 0;
            reconnecting_ = false;
        } else {
            LOGW("Reconnect: no frames after 3s, will retry");
        }
    }

    grpc_client_.stop_streaming();
    reconnecting_ = false;
    running_ = false;
    h264_queue_cv_.notify_all();
    LOGI("gRPC stream loop ended, frames: %llu", (unsigned long long)frame_count_.load());

    // Notify UI that streaming has stopped (connection lost or max retries exceeded)
    if (notify_hwnd_) {
        PostMessageA(notify_hwnd_, kMsgStreamEnded, 0, 0);
    }
}

static bool is_idr_frame(const uint8_t *data, size_t size) {
    // Scan for H264 NAL start codes and check for IDR (type 5)
    size_t pos = 0;
    while (pos + 3 < size) {
        if (data[pos] == 0 && data[pos + 1] == 0) {
            if (data[pos + 2] == 1) {
                // 3-byte start code: 0x00 0x00 0x01
                if (pos + 3 < size) {
                    uint8_t nal_type = data[pos + 3] & 0x1F;
                    if (nal_type == 5) return true;  // IDR
                    if (nal_type == 1) return false;  // Non-IDR slice (P/B frame)
                }
                pos += 4;
                continue;
            }
            if (pos + 4 < size && data[pos + 2] == 0 && data[pos + 3] == 1) {
                // 4-byte start code: 0x00 0x00 0x00 0x01
                uint8_t nal_type = data[pos + 4] & 0x1F;
                if (nal_type == 5) return true;  // IDR
                if (nal_type == 1) return false;  // Non-IDR slice (P/B frame)
                pos += 5;
                continue;
            }
        }
        pos++;
    }
    return false;
}

void ScreenCapture::decode_loop() {
    LOGI("Decode loop started");

    int total_packets = 0;
    while (running_) {
        std::shared_ptr<std::vector<uint8_t>> h264_data;
        {
            std::unique_lock<std::mutex> lock(h264_queue_mutex_);
            h264_queue_cv_.wait(lock, [this] {
                return !h264_queue_.empty() || !running_;
            });
            if (!running_ && h264_queue_.empty()) break;
            if (h264_queue_.empty()) continue;

            // Frame skip: when queue is backlogged, skip to the latest IDR frame
            if (h264_queue_.size() > 3) {
                ssize_t target = -1;
                for (ssize_t i = static_cast<ssize_t>(h264_queue_.size()) - 1; i >= 0; i--) {
                    if (is_idr_frame(h264_queue_[i]->data(), h264_queue_[i]->size())) {
                        target = i;
                        break;
                    }
                }
                if (target > 0) {
                    LOGI("Frame skip: dropping %zu queued frames, jumping to IDR at index %zd",
                         static_cast<size_t>(target), target);
                    h264_queue_.erase(h264_queue_.begin(), h264_queue_.begin() + target);
                } else if (h264_queue_.size() > 5) {
                    // No IDR found but queue is very backlogged - keep last frame only
                    LOGI("Frame skip: no IDR found, dropping %zu queued frames",
                         h264_queue_.size() - 1);
                    h264_queue_.erase(h264_queue_.begin(), h264_queue_.end() - 1);
                }
            }

            h264_data = std::move(h264_queue_.front());
            h264_queue_.pop_front();
        }

        total_packets++;
        bool ok = decoder_.decode(h264_data->data(), h264_data->size());
        if (ok) {
            auto &img = decoder_.image();
            if (img.width > 0 && img.height > 0) {
                uint32_t w = img.width, h = img.height;

                // Use pre-allocated frame pool to avoid per-frame make_shared
                auto &frame = frame_pool_[pool_idx_];
                // If the pool slot is still held by the UI thread, fall back to make_shared
                if (frame.use_count() > 1) {
                    frame = std::make_shared<DecodedImage>();
                }
                std::swap(*frame, decoder_.image_ref());
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_frame_ = frame;
                }
                pool_idx_ = (pool_idx_ + 1) % 3;
                if (w != width_.load() || h != height_.load()) {
                    width_ = w;
                    height_ = h;
                    LOGI("Screen resolution changed: %ux%u", width_.load(), height_.load());
                }
                frame_count_++;
                if (frame_event_) SetEvent(frame_event_);
                if (notify_hwnd_) PostMessageA(notify_hwnd_, kMsgFrameRender, 0, 0);
            }
        } else {
            static int decode_fail_count = 0;
            if (++decode_fail_count <= 3) {
                LOGW("H264 decode failed (%zu bytes)", h264_data->size());
            }
        }
    }

    LOGI("Decode loop ended (total_packets=%d, frames_decoded=%llu)",
         total_packets, (unsigned long long)frame_count_.load());
}

bool ScreenCapture::capture_one_snapshot() {
    char cmd[256];
    if (config_.scale > 1 && width_.load() > 0 && height_.load() > 0) {
        snprintf(cmd, sizeof(cmd),
                 "snapshot_display -f /data/local/tmp/hscrcpy_cap.jpeg -w %u -h %u",
                 width_.load() / config_.scale, height_.load() / config_.scale);
    } else {
        snprintf(cmd, sizeof(cmd), "snapshot_display -f /data/local/tmp/hscrcpy_cap.jpeg");
    }

    std::string output;
    if (!hdc_.shell_command(cmd, &output, 10)) {
        return false;
    }

    unsigned w = 0, h = 0;
    if (sscanf(output.c_str(), "process: display %*d: width %u, height %u", &w, &h) == 2 ||
        sscanf(output.c_str(), " %*[^w]width %u, height %u", &w, &h) == 2) {
        if (w != width_.load() || h != height_.load()) {
            width_ = w;
            height_ = h;
            LOGI("Screen resolution changed: %ux%u", width_.load(), height_.load());
        }
    }

    std::string exe_dir = server_dir_;
    auto sep = exe_dir.find_last_of("\\/");
    if (sep != std::string::npos) exe_dir = exe_dir.substr(0, sep + 1);
    std::string local_file = exe_dir + "hscrcpy_cap.jpeg";

    if (!hdc_.pull_file("/data/local/tmp/hscrcpy_cap.jpeg", local_file)) {
        return false;
    }

    std::ifstream file(local_file, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    auto size = file.tellg();
    if (size <= 0 || size > 10 * 1024 * 1024) return false;

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(buf.data()), size)) return false;
    file.close();
    DeleteFileA(local_file.c_str());

    if (jpeg_decoder_.decode(buf.data(), buf.size())) {
        auto frame = std::make_shared<DecodedImage>(jpeg_decoder_.image());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_frame_ = frame;
        }
        frame_count_++;
        if (frame_event_) SetEvent(frame_event_);
        return true;
    }

    return false;
}

void ScreenCapture::snapshot_loop() {
    LOGI("Snapshot loop started");

    auto interval = std::chrono::milliseconds(1000 / config_.frame_rate);

    while (running_) {
        auto start = std::chrono::steady_clock::now();

        if (!capture_one_snapshot()) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            auto wait = interval - elapsed;
            if (wait > std::chrono::milliseconds(0))
                std::this_thread::sleep_for(wait);
            continue;
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        auto wait = interval - elapsed;
        if (wait > std::chrono::milliseconds(0))
            std::this_thread::sleep_for(wait);
    }

    LOGI("Snapshot loop stopped, frames: %llu", (unsigned long long)frame_count_.load());
}

bool ScreenCapture::get_latest_frame(std::shared_ptr<DecodedImage> &img) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_frame_ || latest_frame_->data.empty()) return false;
    img = latest_frame_;
    return true;
}

} // namespace hscrcpy
