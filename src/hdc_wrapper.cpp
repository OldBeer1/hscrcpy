#include "hdc_wrapper.h"
#include "log.h"
#include <cstdio>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hscrcpy {

HdcWrapper::HdcWrapper(const std::string &serial,
                       const std::string &ip,
                       const std::string &port)
    : serial_(serial), ip_(ip), port_(port) {}

HdcWrapper::~HdcWrapper() {
}

bool HdcWrapper::ensure_connected() {
    if (!is_tcp()) return true;

    std::string result;
    std::string cmd = "tconn " + serial_;
    int ret = run_process(build_hdc_cmd(cmd), &result, 10000);
    if (result.find("Connect OK") != std::string::npos) {
        LOGI("tconn %s: OK", serial_.c_str());
        return true;
    }
    if (result.find("already connected") != std::string::npos ||
        result.find("repeat operation") != std::string::npos) {
        LOGI("tconn %s: already connected", serial_.c_str());
        return true;
    }
    LOGW("tconn %s failed: ret=%d, result='%s'", serial_.c_str(), ret, result.c_str());
    return false;
}

std::string HdcWrapper::build_hdc_cmd(const std::string &subcmd) {
    std::string cmd = "hdc";
    if (!serial_.empty()) {
        cmd += " -t " + serial_;
    }
    cmd += " " + subcmd;
    return cmd;
}

bool HdcWrapper::is_tcp() const {
    return serial_.find(':') != std::string::npos;
}

#ifdef _WIN32
// Persistent shell session - kept as commented reference for future use.
// hdc shell interactive mode via pipes doesn't work on current hdc/HarmonyOS
// versions. All shell commands use optimized direct CreateProcess instead.
#else
#endif

bool HdcWrapper::get_screen_resolution(uint32_t &width, uint32_t &height) {
    std::string output;
    if (!shell_command("hidumper -s RenderService -a screen", &output, 10)) {
        LOGW("hidumper failed, trying display manager");
        return false;
    }

    std::istringstream iss(output);
    std::string line;
    bool found_physical = false;
    while (std::getline(iss, line)) {
        if (line.find("isvirtual=true") != std::string::npos ||
            line.find("isVirtual=true") != std::string::npos) {
            found_physical = false;
            continue;
        }
        if (line.find("isvirtual=false") != std::string::npos ||
            line.find("isVirtual=false") != std::string::npos) {
            found_physical = true;
        }
        if (!found_physical) continue;

        unsigned w = 0, h = 0;
        if (line.find("render size:") != std::string::npos) {
            auto pos = line.find("render size:");
            if (sscanf(line.c_str() + pos, "render size: %ux%u", &w, &h) == 2) {
                width = w;
                height = h;
                LOGI("Physical screen resolution (render size): %ux%u", w, h);
                return true;
            }
        }
        if (line.find("render resolution=") != std::string::npos) {
            auto pos = line.find("render resolution=");
            if (sscanf(line.c_str() + pos, "render resolution=%ux%u", &w, &h) == 2) {
                width = w;
                height = h;
                LOGI("Physical screen resolution (render resolution): %ux%u", w, h);
                return true;
            }
        }
        if (line.find("physical screen resolution:") != std::string::npos) {
            auto pos = line.find("physical screen resolution:");
            if (sscanf(line.c_str() + pos, "physical screen resolution: %ux%u", &w, &h) == 2) {
                width = w;
                height = h;
                LOGI("Physical screen resolution (physical): %ux%u", w, h);
                return true;
            }
        }
        if (line.find("physical resolution=") != std::string::npos) {
            auto pos = line.find("physical resolution=");
            if (sscanf(line.c_str() + pos, "physical resolution=%ux%u", &w, &h) == 2) {
                width = w;
                height = h;
                LOGI("Physical screen resolution (physical resolution): %ux%u", w, h);
                return true;
            }
        }
    }

    LOGW("Could not parse physical screen resolution from hidumper output");
    return false;
}

static std::string find_hdc_exe() {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string exe_dir(exe_path);
    auto sep = exe_dir.find_last_of("\\/");
    if (sep != std::string::npos) exe_dir = exe_dir.substr(0, sep + 1);

    std::string hdc_exe_path = exe_dir + "\\hdc.exe";
    std::string hdc_path = exe_dir + "\\hdc";
    const char *candidates[] = {
        hdc_exe_path.c_str(),
        hdc_path.c_str(),
        "hdc.exe",
        "hdc"
    };
    for (auto *c : candidates) {
        if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) {
            return c;
        }
    }
    char pb[MAX_PATH];
    if (SearchPathA(NULL, "hdc.exe", NULL, MAX_PATH, pb, NULL) > 0) return pb;
    if (SearchPathA(NULL, "hdc", NULL, MAX_PATH, pb, NULL) > 0) return pb;
    return "";
}

int HdcWrapper::run_process(const std::string &cmd, std::string *output, int timeout_ms) {
    LOGI("CMD: %s (timeout=%d ms)", cmd.c_str(), timeout_ms);
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE hRead, hWrite;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string exe_dir(exe_path);
    auto last_sep = exe_dir.find_last_of("\\/");
    if (last_sep != std::string::npos) exe_dir = exe_dir.substr(0, last_sep + 1);

    std::string full_cmd;

    // For hdc commands, run directly (skip cmd.exe /C) for speed
    static std::string s_hdc_path;
    if (s_hdc_path.empty()) s_hdc_path = find_hdc_exe();
    bool ran_direct = false;
    if (!s_hdc_path.empty() && cmd.find("hdc") == 0) {
        full_cmd = s_hdc_path + cmd.substr(4);
        if (CreateProcessA(NULL, const_cast<char *>(full_cmd.c_str()),
                            NULL, NULL, TRUE, CREATE_NO_WINDOW,
                            NULL, NULL, &si, &pi)) {
            ran_direct = true;
        } else {
            LOGD("Direct hdc failed (err=%lu), falling back to cmd.exe /C", GetLastError());
        }
    }

    if (!ran_direct) {
        full_cmd = "cmd.exe /C " + cmd;
        if (!CreateProcessA(NULL, const_cast<char *>(full_cmd.c_str()),
                            NULL, NULL, TRUE, CREATE_NO_WINDOW,
                            NULL, exe_dir.c_str(), &si, &pi)) {
            LOGE("CreateProcess failed for: %s (err=%lu)", cmd.c_str(), GetLastError());
            CloseHandle(hRead);
            CloseHandle(hWrite);
            return -1;
        }
    }

    CloseHandle(hWrite);

    std::string result;
    char buf[8192];
    DWORD bytesRead = 0;

    struct ReadCtx {
        HANDLE hRead;
        std::string *result;
        char *buf;
        bool done;
    } ctx = {hRead, &result, buf, false};

    HANDLE hThread = CreateThread(NULL, 0,
        [](LPVOID param) -> DWORD {
            auto *c = reinterpret_cast<ReadCtx *>(param);
            DWORD bytes = 0;
            while (ReadFile(c->hRead, c->buf, 8192, &bytes, NULL) && bytes > 0) {
                c->result->append(c->buf, bytes);
            }
            c->done = true;
            return 0;
        }, &ctx, 0, NULL);

    DWORD wait = static_cast<DWORD>(timeout_ms);
    DWORD waitResult = WaitForSingleObject(pi.hProcess, wait);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        LOGW("Process timed out: %s (%d ms)", cmd.c_str(), timeout_ms);
    }

    WaitForSingleObject(hThread, 3000);
    CloseHandle(hThread);
    CloseHandle(hRead);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (output) *output = std::move(result);
    return static_cast<int>(exitCode);
#else
    (void)timeout_ms;
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp) {
        LOGE("Failed to run: %s", cmd.c_str());
        return -1;
    }
    char buf[8192];
    std::string result;
    while (auto n = std::fread(buf, 1, sizeof(buf), fp)) {
        if (output) result.append(buf, n);
    }
    if (output) *output = std::move(result);
    int status = pclose(fp);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::vector<HdcDevice> HdcWrapper::list_devices() {
    std::string output;
    if (run_process("hdc list targets", &output, 10000) != 0) {
        run_process("hdc devices", &output, 10000);
    }

    std::vector<HdcDevice> devices;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.erase(line.begin());
        if (line.empty() || line.front() == '[') continue;
        if (line.find("Empty") != std::string::npos) continue;
        if (line.find("failed") != std::string::npos) continue;

        auto tab_pos = line.find('\t');
        std::string serial = (tab_pos != std::string::npos) ? line.substr(0, tab_pos) : line;
        std::string state = (tab_pos != std::string::npos) ? line.substr(tab_pos + 1) : "unknown";

        auto colon_pos = serial.find(':');
        if (colon_pos != std::string::npos) {
            std::string ip = serial.substr(0, colon_pos);
            std::string port = serial.substr(colon_pos + 1);
            devices.push_back({serial, state, ip, port});
        } else {
            devices.push_back({serial, state, "127.0.0.1", "8710"});
        }
    }
    return devices;
}

bool HdcWrapper::forward_tcp(uint16_t local_port, uint16_t remote_port) {
    char subcmd[256];
    snprintf(subcmd, sizeof(subcmd), "fport tcp:%u tcp:%u", local_port, remote_port);
    std::string result;
    int ret = run_process(build_hdc_cmd(subcmd), &result, 10000);
    bool ok = (ret == 0) && (result.find("OK") != std::string::npos) && (result.find("Fail") == std::string::npos);
    LOGD("forward_tcp result: %s (ret=%d, ok=%s)", result.c_str(), ret, ok ? "true" : "false");
    return ok;
}

bool HdcWrapper::forward_localabstract(uint16_t local_port, const std::string &socket_name) {
    char subcmd[256];
    snprintf(subcmd, sizeof(subcmd), "fport tcp:%u localabstract:%s", local_port, socket_name.c_str());
    std::string result;
    int ret = run_process(build_hdc_cmd(subcmd), &result, 10000);
    bool ok = (ret == 0) && (result.find("OK") != std::string::npos) && (result.find("Fail") == std::string::npos);
    LOGD("forward_localabstract result: %s (ret=%d, ok=%s)", result.c_str(), ret, ok ? "true" : "false");
    return ok;
}

bool HdcWrapper::forward_remove(uint16_t local_port, bool is_abstract, const std::string &socket_name) {
    std::string output;
    run_process(build_hdc_cmd("fport ls"), &output, 10000);
    std::istringstream iss(output);
    std::string line;
    char local_str[32];
    snprintf(local_str, sizeof(local_str), "tcp:%u", local_port);
    while (std::getline(iss, line)) {
        if (line.find(local_str) == std::string::npos) continue;
        auto pos = line.find("tcp:");
        if (pos == std::string::npos) pos = line.find("localabstract:");
        if (pos == std::string::npos) continue;
        std::string entry = line.substr(pos);
        while (!entry.empty() && (entry.back() == '\r' || entry.back() == '\n' || entry.back() == ' '))
            entry.pop_back();
        std::string rm_cmd = "fport rm " + entry;
        return run_process(build_hdc_cmd(rm_cmd), nullptr, 5000) == 0;
    }
    return true;
}

bool HdcWrapper::forward_remove_all() {
    std::string output;
    run_process(build_hdc_cmd("fport ls"), &output, 10000);
    std::istringstream iss(output);
    std::string line;
    bool any_removed = false;
    while (std::getline(iss, line)) {
        auto pos = line.find("tcp:");
        if (pos == std::string::npos) continue;
        std::string entry = line.substr(pos);
        while (!entry.empty() && (entry.back() == '\r' || entry.back() == '\n' || entry.back() == ' '))
            entry.pop_back();
        std::string rm_cmd = "fport rm " + entry;
        if (run_process(build_hdc_cmd(rm_cmd), nullptr, 5000) == 0) {
            any_removed = true;
        }
    }
    return any_removed;
}

int HdcWrapper::forward_count() {
    std::string output;
    run_process(build_hdc_cmd("fport ls"), &output, 10000);
    int count = 0;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("tcp:") != std::string::npos) count++;
    }
    return count;
}

bool HdcWrapper::cleanup_stale_forwards() {
    std::string output;
    run_process(build_hdc_cmd("fport ls"), &output, 10000);
    std::istringstream iss(output);
    std::string line;
    int removed = 0;
    while (std::getline(iss, line)) {
        auto pos = line.find("tcp:");
        if (pos == std::string::npos) continue;
        std::string entry = line.substr(pos);
        while (!entry.empty() && (entry.back() == '\r' || entry.back() == '\n' || entry.back() == ' '))
            entry.pop_back();

        bool is_our_forward = entry.find("scrcpy_grpc_socket") != std::string::npos ||
                              entry.find("tcp:5000") != std::string::npos ||
                              entry.find("uitest_socket") != std::string::npos ||
                              entry.find("tcp:8012") != std::string::npos;
        if (!is_our_forward) continue;

        std::string rm_cmd = "fport rm " + entry;
        if (run_process(build_hdc_cmd(rm_cmd), nullptr, 5000) == 0) {
            removed++;
        }
    }
    if (removed > 0) {
        LOGI("Cleaned up %d stale forwards for device %s", removed, serial_.c_str());
    }
    return removed > 0;
}

bool HdcWrapper::shell_command(const std::string &cmd, std::string *output, int timeout_sec) {
    return run_process(build_hdc_cmd("shell " + cmd), output, timeout_sec * 1000) == 0;
}

bool HdcWrapper::push_file(const std::string &local_path, const std::string &remote_path) {
    std::string subcmd = "file send \"" + local_path + "\" " + remote_path;
    std::string result;
    int ret = run_process(build_hdc_cmd(subcmd), &result, 30000);
    LOGD("push_file: %s -> %s, result: %s", local_path.c_str(), remote_path.c_str(), result.c_str());
    return ret == 0;
}

bool HdcWrapper::pull_file(const std::string &remote_path, const std::string &local_path) {
    std::string subcmd = "file recv " + remote_path + " \"" + local_path + "\"";
    std::string result;
    int ret = run_process(build_hdc_cmd(subcmd), &result, 30000);
    LOGD("pull_file: %s -> %s, result: %s", remote_path.c_str(), local_path.c_str(), result.c_str());
    return ret == 0;
}

} // namespace hscrcpy
