#ifndef HSCRCPY_COMMON_LOG_H
#define HSCRCPY_COMMON_LOG_H

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

// Verbose flag — set from CLI (-v). LOGV outputs only when this is true.
extern bool g_verbose;

static inline void log_to_file(const char *msg, int level) {
#ifdef _WIN32
    static HANDLE hLog = INVALID_HANDLE_VALUE;
    if (hLog == INVALID_HANDLE_VALUE) {
        char exe_path[MAX_PATH];
        GetModuleFileNameA(NULL, exe_path, MAX_PATH);
        std::string path(exe_path);
        auto pos = path.find_last_of("\\/");
        if (pos != std::string::npos) path = path.substr(0, pos + 1);
        path += "hscrcpy.log";
        // On startup (first write), if the log exceeds 5MB, delete and recreate it.
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) {
            ULARGE_INTEGER sz;
            sz.LowPart = fa.nFileSizeLow;
            sz.HighPart = fa.nFileSizeHigh;
            if (sz.QuadPart > 5ull * 1024 * 1024) DeleteFileA(path.c_str());
        }
        hLog = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hLog != INVALID_HANDLE_VALUE) {
            SetFilePointer(hLog, 0, NULL, FILE_END);
        }
    }
    if (hLog != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hLog, msg, (DWORD)strlen(msg), &written, NULL);
        WriteFile(hLog, "\r\n", 2, &written, NULL);
        // Only flush on errors - avoids synchronous disk I/O on every LOGI/LOGW
        if (level >= 2) FlushFileBuffers(hLog);
    }
#endif
}

#define LOGI(fmt, ...) do { char _buf[1024]; snprintf(_buf, sizeof(_buf), "[I] " fmt, ##__VA_ARGS__); log_to_file(_buf, 0); } while(0)
#define LOGW(fmt, ...) do { char _buf[1024]; snprintf(_buf, sizeof(_buf), "[W] " fmt, ##__VA_ARGS__); log_to_file(_buf, 1); } while(0)
#define LOGE(fmt, ...) do { char _buf[1024]; snprintf(_buf, sizeof(_buf), "[E] " fmt, ##__VA_ARGS__); log_to_file(_buf, 2); } while(0)
#define LOGV(fmt, ...) do { if (g_verbose) { char _buf[1024]; snprintf(_buf, sizeof(_buf), "[V] " fmt, ##__VA_ARGS__); log_to_file(_buf, 0); } } while(0)
#define LOGD(fmt, ...) do { } while(0)
#define LOGI_ONCE(fmt, ...) do { static bool _once = false; if (!_once) { _once = true; LOGI(fmt, ##__VA_ARGS__); } } while(0)

#endif
