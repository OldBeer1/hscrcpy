#include "screen_capture.h"
#include "log.h"
#include "protocol.h"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hscrcpy {

ScreenCapture::ScreenCapture(HdcWrapper &hdc, uint32_t fps)
    : hdc_(hdc), fps_(fps) {
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

std::string ScreenCapture::get_uitest_version() {
    std::string output;
    hdc_.shell_command("/system/bin/uitest --version", &output, 5);
    std::string ver;
    for (auto &c : output) {
        if ((c >= '0' && c <= '9') || c == '.') ver += c;
        else if (!ver.empty()) break;
    }
    LOGI("uitest version: '%s' (raw: '%s')", ver.c_str(), output.c_str());
    return ver;
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

std::string ScreenCapture::get_device_so_md5() {
    std::string output;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "md5sum %s", kDeviceSoPath);
    hdc_.shell_command(cmd, &output, 8);
    auto pos = output.find(' ');
    if (pos != std::string::npos) return output.substr(0, pos);
    auto pos2 = output.find('\t');
    if (pos2 != std::string::npos) return output.substr(0, pos2);
    pos2 = output.find('\n');
    if (pos2 != std::string::npos) return output.substr(0, pos2);
    while (!output.empty() && (output.back() == '\r' || output.back() == '\n' || output.back() == ' '))
        output.pop_back();
    return output;
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
    bool old_version = version_compare_result(uitest_version_, "6.0.2.1") == "less";

    std::vector<std::string> candidates;
    if (old_version) {
        candidates = {
            "libscrcpy_server1.z.so",
            "libscrcpy_server0.z.so",
            "libscrcpy_server2.z.so",
            "libscrcpy_server3.z.so",
        };
    } else {
        candidates = {
            "libscrcpy_server_unix_6.3.1-20260113.z.so",
            "libscrcpy_server_unix_6.4-20260113.z.so",
            "libscrcpy_server_5.10-20260114.z.so",
        };
    }

    if (!device_md5.empty()) {
        for (auto &name : candidates) {
            std::string path = server_dir_ + "\\" + name;
            if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            std::string local_md5 = get_local_so_md5(path);
            if (!local_md5.empty() && local_md5 == device_md5) {
                LOGI("MD5 matched: %s", name.c_str());
                return path;
            }
        }
    }

    for (auto &name : candidates) {
        std::string path = server_dir_ + "\\" + name;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }

    WIN32_FIND_DATAA fd;
    std::string pattern = server_dir_ + "\\*.z.so";
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        std::string result;
        do {
            if (old_version) {
                if (strstr(fd.cFileName, "unix") == nullptr && strstr(fd.cFileName, "emulator") == nullptr) {
                    result = server_dir_ + "\\" + fd.cFileName;
                    break;
                }
            } else {
                if (strstr(fd.cFileName, "unix") != nullptr) {
                    result = server_dir_ + "\\" + fd.cFileName;
                    break;
                }
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

bool ScreenCapture::push_server_so() {
    std::string device_md5 = get_device_so_md5();
    std::string local_so = find_matching_so_with_md5(device_md5);
    if (local_so.empty()) {
        LOGE("No matching server .z.so found in %s", server_dir_.c_str());
        return false;
    }
    LOGI("Using server SO: %s", local_so.c_str());

    std::string local_md5 = get_local_so_md5(local_so);
    LOGI("Device SO md5: %s, Local SO md5: %s", device_md5.c_str(), local_md5.c_str());

    if (!device_md5.empty() && device_md5 == local_md5) {
        LOGI("Server SO already up to date on device");
        return true;
    }

    LOGI("Pushing %s to %s", local_so.c_str(), kDeviceSoPath);
    if (!hdc_.push_file(local_so, kDeviceSoPath)) {
        LOGE("Failed to push server SO");
        return false;
    }
    return true;
}

bool ScreenCapture::push_agent_so() {
    std::string local_agent = find_matching_agent_so();
    if (local_agent.empty()) {
        LOGW("No matching uitest agent .so found");
        return false;
    }
    LOGI("Using agent SO: %s", local_agent.c_str());

    std::string check_output;
    hdc_.shell_command("cat /data/local/tmp/agent.so | grep -a UITEST_AGENT_LIBRARY", &check_output, 5);
    if (check_output.find("UITEST_AGENT_LIBRARY") != std::string::npos) {
        LOGI("Agent already exists on device");
        return true;
    }

    LOGI("Pushing agent %s to %s", local_agent.c_str(), kDeviceAgentPath);
    return hdc_.push_file(local_agent, kDeviceAgentPath);
}

bool ScreenCapture::start_uitest_with_extension() {
    std::string ps_output;
    hdc_.shell_command("\"ps -ef | grep extension-name | grep -v grep\"", &ps_output, 5);

    std::vector<std::string> pids;
    std::istringstream iss(ps_output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("extension-name") == std::string::npos) continue;
        if (line.find("grep") != std::string::npos) continue;
        std::istringstream ls(line);
        std::string user, pid_str;
        if (ls >> user >> pid_str) {
            bool all_digit = true;
            for (char c : pid_str) { if (c < '0' || c > '9') { all_digit = false; break; } }
            if (all_digit && !pid_str.empty()) {
                pids.push_back(pid_str);
            }
        }
    }

    if (pids.empty()) {
        LOGI("No existing uitest extension process, starting new one");
    } else {
        LOGI("Found %zu existing uitest extension process(es), killing with SIGKILL", pids.size());
        std::string kill_cmd = "kill -9";
        for (auto &p : pids) kill_cmd += " " + p;
        hdc_.shell_command(kill_cmd, nullptr, 3);
        Sleep(1000);
    }

    std::string cmd;
    if (use_abstract_socket_) {
        cmd = "/system/bin/uitest start-daemon singleness --extension-name libscreen_casting.z.so -scale 1 -frameRate 30 -bitRate 4000000 " +
              std::string(kGrpcSocketName) + " -iFrameInterval 2000 &";
    } else {
        cmd = "/system/bin/uitest start-daemon singleness --extension-name libscreen_casting.z.so -scale 1 -frameRate 30 -bitRate 4000000 -p 5000 -iFrameInterval 2000 &";
    }
    LOGI("Starting uitest: %s", cmd.c_str());
    hdc_.shell_command(cmd, nullptr, 5);
    Sleep(3000);

    std::string check;
    hdc_.shell_command("\"ps -ef | grep extension-name | grep -v grep\"", &check, 5);
    if (check.find("extension-name") != std::string::npos) {
        LOGI("uitest extension process confirmed running");
        uitest_port_ = kDefaultVideoPort;
        return true;
    }

    LOGE("uitest extension process not found after start");
    return false;
}

bool ScreenCapture::setup_port_forward() {
    hdc_.forward_remove_all();
    Sleep(200);

    for (uint16_t port = kDefaultLocalBasePort; port < kDefaultLocalBasePort + 100; port += 2) {
        local_video_port_ = port;
        local_control_port_ = port + 1;

        bool ok;
        if (use_abstract_socket_) {
            ok = hdc_.forward_localabstract(local_video_port_, kGrpcSocketName);
        } else {
            char remote[32];
            snprintf(remote, sizeof(remote), "tcp:%u", uitest_port_);
            ok = hdc_.forward_tcp(local_video_port_, uitest_port_);
        }
        if (ok) {
            char remote_info[64];
            if (use_abstract_socket_) {
                snprintf(remote_info, sizeof(remote_info), "%s", kGrpcSocketName);
            } else {
                snprintf(remote_info, sizeof(remote_info), "tcp:%u", uitest_port_);
            }
            LOGI("Port forward: local %u -> %s", local_video_port_, remote_info);
            break;
        }
        local_video_port_ = 0;
    }

    if (local_video_port_ == 0) {
        LOGE("Failed to set up port forward");
        return false;
    }
    return true;
}

bool ScreenCapture::setup_grpc_stream() {
    LOGI("setup_grpc_stream: serial='%s'", hdc_.serial().c_str());
    uitest_version_ = get_uitest_version();
    if (uitest_version_.empty()) {
        LOGW("Could not get uitest version, assuming old version");
        uitest_version_ = "4.0.0.0";
    }

    use_abstract_socket_ = version_compare_result(uitest_version_, "6.0.2.1") != "less";
    LOGI("uitest version: %s, use_abstract_socket: %s",
         uitest_version_.c_str(), use_abstract_socket_ ? "true" : "false");

    if (!decoder_.init()) {
        LOGW("H264 decoder not available (FFmpeg DLLs not found), skipping gRPC stream");
        LOGW("To enable gRPC virtual screen streaming, download FFmpeg shared DLLs from:");
        LOGW("https://github.com/BtbN/FFmpeg-Builds/releases");
        LOGW("Copy avcodec-62.dll, avutil-60.dll, swscale-9.dll, swresample-6.dll to the exe directory");
        return false;
    }

    if (!push_server_so()) {
        LOGE("Failed to push server SO");
        return false;
    }

    if (!start_uitest_with_extension()) {
        LOGE("Failed to start uitest with extension");
        return false;
    }

    if (!setup_port_forward()) {
        LOGE("Failed to setup port forward");
        return false;
    }

    Sleep(500);

    bool connected = false;
    for (int retry = 0; retry < 5; retry++) {
        LOGI("gRPC connect attempt %d to 127.0.0.1:%u", retry + 1, local_video_port_);
        if (grpc_client_.connect("127.0.0.1", local_video_port_)) {
            connected = true;
            LOGI("gRPC connected successfully");
            break;
        }
        if (retry == 1) {
            LOGI("Retrying uitest extension start...");
            std::string cmd;
            if (use_abstract_socket_) {
                cmd = "/system/bin/uitest start-daemon singleness --extension-name libscreen_casting.z.so -scale 1 -frameRate 30 -bitRate 4000000 " +
                      std::string(kGrpcSocketName) + " -iFrameInterval 2000 &";
            } else {
                cmd = "/system/bin/uitest start-daemon singleness --extension-name libscreen_casting.z.so -scale 1 -frameRate 30 -bitRate 4000000 -p 5000 -iFrameInterval 2000 &";
            }
            hdc_.shell_command(cmd, nullptr, 5);
            Sleep(3000);
        } else {
            Sleep(1000);
        }
    }

    if (!connected) {
        LOGE("Failed to connect gRPC server after retries");
        return false;
    }

    mode_ = CaptureMode::kGrpcStream;
    LOGI("gRPC stream setup complete, starting streaming");

    hdc_.shell_command("power-shell wakeup", nullptr, 5);
    LOGI("Sent power-shell wakeup (before streaming)");

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
        LOGI("Screen resolution from snapshot: %ux%u", width_, height_);
    }

    if (width_ == 0) {
        width_ = 1260;
        height_ = 2720;
        LOGI("Using default resolution: %ux%u", width_, height_);
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
        if (!setup_snapshot_fallback()) {
            LOGE("Snapshot fallback also failed");
            return false;
        }
    }

    running_ = true;
    if (mode_ == CaptureMode::kGrpcStream) {
        thread_ = std::thread(&ScreenCapture::grpc_stream_loop, this);
    } else {
        thread_ = std::thread(&ScreenCapture::snapshot_loop, this);
    }
    return true;
}

void ScreenCapture::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();

    grpc_client_.disconnect();
    decoder_.cleanup();

    if (local_video_port_ > 0) {
        hdc_.forward_remove(local_video_port_, use_abstract_socket_,
                            use_abstract_socket_ ? kGrpcSocketName : "");
    }
}

void ScreenCapture::grpc_stream_loop() {
    LOGI("gRPC stream loop started");

    auto on_frame = [this](const std::vector<uint8_t> &h264_data) {
        bool ok = decoder_.decode(h264_data.data(), h264_data.size());
        if (ok) {
            auto &img = decoder_.image();
            if (img.width > 0 && img.height > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_frame_ = img;
                if (width_ == 0) {
                    width_ = img.width;
                    height_ = img.height;
                    LOGI("Screen resolution from stream: %ux%u", width_, height_);
                }
                frame_count_++;
            } else {
                LOGW("Decoded frame but width=0 height=0");
            }
        } else {
            LOGW("H264 decode failed for %zu bytes", h264_data.size());
        }
    };

    if (!grpc_client_.start_streaming(on_frame)) {
        LOGE("Failed to start gRPC streaming");
        running_ = false;
        return;
    }

    LOGI("gRPC streaming started (IDR already requested)");

    while (running_ && grpc_client_.is_connected()) {
        Sleep(100);
    }

    grpc_client_.stop_streaming();
    LOGI("gRPC stream loop ended, frames: %llu", (unsigned long long)frame_count_.load());
}

bool ScreenCapture::capture_one_snapshot() {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "snapshot_display -f /data/local/tmp/hscrcpy_cap.jpeg");

    std::string output;
    if (!hdc_.shell_command(cmd, &output, 10)) {
        return false;
    }

    if (width_ == 0) {
        unsigned w = 0, h = 0;
        if (sscanf(output.c_str(), "process: display %*d: width %u, height %u", &w, &h) == 2 ||
            sscanf(output.c_str(), " %*[^w]width %u, height %u", &w, &h) == 2) {
            width_ = w;
            height_ = h;
        }
    }

    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string exe_dir(exe_path);
    auto last_sep = exe_dir.find_last_of("\\/");
    if (last_sep != std::string::npos) exe_dir = exe_dir.substr(0, last_sep + 1);
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
        std::lock_guard<std::mutex> lock(mutex_);
        latest_frame_ = jpeg_decoder_.image();
        frame_count_++;
        return true;
    }

    return false;
}

void ScreenCapture::snapshot_loop() {
    LOGI("Snapshot loop started");

    auto interval = std::chrono::milliseconds(1000 / fps_);

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

bool ScreenCapture::get_latest_frame(DecodedImage &img) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_frame_.data.empty()) return false;
    img = latest_frame_;
    return true;
}

} // namespace hscrcpy
