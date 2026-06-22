#include "file_manager_window.h"
#include "log.h"
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <sstream>
#include <algorithm>
#include <set>

// Check if shell command output contains error indicators.
// hdc shell returns exit code 0 even when the inner command fails,
// so we must inspect the output text for common error patterns.
static bool shell_output_has_error(const std::string &output) {
    if (output.empty()) return false;
    // Common error patterns from cp, mv, rm, mkdir, touch, etc.
    static const char *patterns[] = {
        "Permission denied",
        "permission denied",
        "Read-only",
        "read-only",
        "No such file",
        "not found",
        "Operation not permitted",
        "Cannot",
        "failed",
        "Error",
    };
    for (auto &p : patterns) {
        if (output.find(p) != std::string::npos) return true;
    }
    return false;
}

namespace hscrcpy {

// ── Input dialog helper ──────────────────────────────────

struct InputDlgData {
    std::wstring result;
    bool ok;
    const wchar_t *title;
    const wchar_t *prompt;
};

static LRESULT CALLBACK InputDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *data = reinterpret_cast<InputDlgData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto *cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        data = reinterpret_cast<InputDlgData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);

        SetWindowTextW(hwnd, data->title ? data->title : L"Input");

        CreateWindowW(L"STATIC", data->prompt ? data->prompt : L"Name:",
            WS_CHILD | WS_VISIBLE, 12, 14, 270, 18, hwnd, NULL, cs->hInstance, NULL);
        CreateWindowExW(0, L"EDIT", data->result.c_str(),
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            12, 38, 270, 24, hwnd, (HMENU)3001, cs->hInstance, NULL);
        CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            110, 72, 80, 26, hwnd, (HMENU)IDOK, cs->hInstance, NULL);
        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            200, 72, 80, 26, hwnd, (HMENU)IDCANCEL, cs->hInstance, NULL);

        HFONT hf = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)hf, TRUE);
        EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE;
        }, (LPARAM)hf);

        // Focus edit control
        HWND edit = GetDlgItem(hwnd, 3001);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[256];
            GetWindowTextW(GetDlgItem(hwnd, 3001), buf, 256);
            data->result = buf;
            data->ok = true;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (LOWORD(wp) == IDCANCEL) {
            data->ok = false;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool FileManagerWindow::show_input_dialog(const wchar_t *title, const wchar_t *prompt, std::wstring &out_value) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = InputDlgProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HScrcpyInputDlgClass";
        RegisterClassExW(&wc);
        reg = true;
    }

    InputDlgData data;
    data.result = out_value;
    data.ok = false;
    data.title = title;
    data.prompt = prompt;

    HWND owner = g_hwnd;
    RECT orr;
    GetWindowRect(owner, &orr);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"HScrcpyInputDlgClass", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        orr.left + (orr.right - orr.left - 300) / 2,
        orr.top + (orr.bottom - orr.top - 130) / 2,
        300, 130, owner, NULL, g_inst, &data);

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    // Modal message loop
    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, NULL, 0, 0)) {
        if (!IsWindow(dlg)) break;
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    EnableWindow(owner, TRUE);
    SetFocus(owner);

    if (data.ok) {
        out_value = data.result;
        return true;
    }
    return false;
}

// ── Helpers ──────────────────────────────────────────────

static std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (len <= 0) return L"";
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

static std::string wide_to_utf8(const std::wstring &ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    if (len <= 0) return "";
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, NULL, NULL);
    return s;
}

static std::string format_size(long long bytes) {
    char buf[32];
    if (bytes < 1024) snprintf(buf, sizeof(buf), "%lld B", bytes);
    else if (bytes < 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    else snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

static std::string ensure_dir_sep(const std::string &path) {
    if (path.empty()) return "/";
    if (path.back() == '/') return path;
    return path + "/";
}

// Shell-escape a path: wrap in single quotes, escape internal single quotes
static std::string shell_escape(const std::string &s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    r += "'";
    return r;
}

static HICON load_icon_file(const char *rel_path, int size,
                             BYTE tint_r = 0, BYTE tint_g = 0, BYTE tint_b = 0,
                             bool tint = false) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string dir(exe_path);
    auto sep = dir.find_last_of("\\/");
    if (sep != std::string::npos) dir = dir.substr(0, sep + 1);
    std::string path = dir + rel_path;

    int wlen = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, NULL, 0);
    std::wstring wpath(wlen - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &wpath[0], wlen);

    HICON icon = NULL;
    Gdiplus::Bitmap *src = Gdiplus::Bitmap::FromFile(wpath.c_str(), FALSE);
    if (!src || src->GetLastStatus() != Gdiplus::Ok) {
        LOGE("load_icon_file: failed to load %s", path.c_str());
        delete src;
        return NULL;
    }
    int sw = (int)src->GetWidth(), sh = (int)src->GetHeight();
    if (sw <= 0 || sh <= 0) { delete src; return NULL; }

    int dw = size, dh = size;
    if (sw * dh > sh * dw) dh = (int)(size * (double)sh / sw);
    else dw = (int)(size * (double)sw / sh);
    int ox = (size - dw) / 2, oy = (size - dh) / 2;

    Gdiplus::Bitmap *dst = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics g(dst);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    g.DrawImage(src, ox, oy, dw, dh);
    delete src;

    if (tint) {
        // Read back drawn pixels and apply tint
        Gdiplus::BitmapData bd = {};
        Gdiplus::Rect r(0, 0, size, size);
        dst->LockBits(&r, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd);
        for (int y = 0; y < size; y++) {
            BYTE *p = (BYTE *)bd.Scan0 + y * bd.Stride;
            for (int x = 0; x < size; x++) {
                BYTE a = p[3];
                if (a > 0) {
                    p[2] = (BYTE)(tint_r * a / 255);  // R
                    p[1] = (BYTE)(tint_g * a / 255);  // G
                    p[0] = (BYTE)(tint_b * a / 255);  // B
                }
                p += 4;
            }
        }
        dst->UnlockBits(&bd);
    }

    dst->GetHICON(&icon);
    delete dst;
    return icon;
}

// ── Statics ──────────────────────────────────────────────

HWND FileManagerWindow::g_hwnd = nullptr;
HWND FileManagerWindow::g_address_edit = nullptr;
HWND FileManagerWindow::g_folder_icon = nullptr;
HWND FileManagerWindow::g_go_btn = nullptr;
HWND FileManagerWindow::g_back_btn = nullptr;
HWND FileManagerWindow::g_search_edit = nullptr;
HWND FileManagerWindow::g_search_btn = nullptr;
HWND FileManagerWindow::g_import_btn = nullptr;
HWND FileManagerWindow::g_export_btn = nullptr;
HWND FileManagerWindow::g_new_btn = nullptr;
HWND FileManagerWindow::g_delete_btn = nullptr;
HWND FileManagerWindow::g_copy_btn = nullptr;
HWND FileManagerWindow::g_paste_btn = nullptr;
HWND FileManagerWindow::g_rename_btn = nullptr;
HWND FileManagerWindow::g_file_list = nullptr;
HWND FileManagerWindow::g_status_label = nullptr;
HINSTANCE FileManagerWindow::g_inst = nullptr;
HWND FileManagerWindow::g_parent = nullptr;
int FileManagerWindow::g_panel_w = 420;

std::string FileManagerWindow::g_current_dir;
std::vector<FileEntry> FileManagerWindow::g_entries;
std::vector<std::string> FileManagerWindow::g_history;
FileClipboard FileManagerWindow::g_clipboard;

FileManagerWindow::OnShellCommand FileManagerWindow::g_on_shell;
FileManagerWindow::OnPushFile FileManagerWindow::g_on_push;
FileManagerWindow::OnPullFile FileManagerWindow::g_on_pull;
FileManagerWindow::OnBusy FileManagerWindow::g_on_busy;
bool FileManagerWindow::g_busy = false;

HICON FileManagerWindow::g_icon_folder = nullptr;
HICON FileManagerWindow::g_icon_file = nullptr;
HICON FileManagerWindow::g_icon_link = nullptr;
HIMAGELIST FileManagerWindow::g_img_list = nullptr;
HWND FileManagerWindow::g_toast_hwnd = nullptr;

static const wchar_t *FM_CLASS_W = L"HScrcpyFileManagerClass";

// ── Edit subclass for Enter key ──────────────────────────

static WNDPROC g_addr_orig_proc = nullptr;
static WNDPROC g_search_orig_proc = nullptr;

static LRESULT CALLBACK AddrEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageW(GetParent(hwnd), WM_COMMAND, MAKELONG(2002, BN_CLICKED), 0); // IDC_GO_BTN
        return 0;
    }
    return CallWindowProcW(g_addr_orig_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageW(GetParent(hwnd), WM_COMMAND, MAKELONG(2006, BN_CLICKED), 0); // IDC_SEARCH_BTN
        return 0;
    }
    return CallWindowProcW(g_search_orig_proc, hwnd, msg, wp, lp);
}

// ── Public API ───────────────────────────────────────────

void FileManagerWindow::create(HINSTANCE inst, HWND parent, int width) {
    g_inst = inst;
    g_parent = parent;
    g_panel_w = width;

    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX icex = {sizeof(icex), ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&icex);

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = FM_CLASS_W;
        RegisterClassExW(&wc);
        registered = true;
    }
}

void FileManagerWindow::show(OnShellCommand on_shell, OnPushFile on_push, OnPullFile on_pull, OnBusy on_busy) {
    g_on_shell = on_shell;
    g_on_push = on_push;
    g_on_pull = on_pull;
    g_on_busy = on_busy;

    if (!g_hwnd) {
        g_hwnd = CreateWindowExW(0, FM_CLASS_W, L"",
                                  WS_CHILD | WS_BORDER | WS_CLIPCHILDREN,
                                  0, 0, g_panel_w, 100, g_parent, NULL, g_inst, NULL);
    }

    ShowWindow(g_hwnd, SW_SHOW);
    if (g_current_dir.empty()) navigate("/");
    else refresh_list();
}

void FileManagerWindow::hide() {
    if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
}

void FileManagerWindow::destroy() {
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
}

bool FileManagerWindow::is_visible() {
    return g_hwnd && IsWindowVisible(g_hwnd);
}

HWND FileManagerWindow::hwnd() { return g_hwnd; }
int FileManagerWindow::panel_width() { return g_panel_w; }
void FileManagerWindow::set_panel_width(int w) { g_panel_w = w; }

void FileManagerWindow::reposition(int x, int y, int w, int h) {
    if (g_hwnd) MoveWindow(g_hwnd, x, y, w, h, TRUE);
}

// ── ls -al parser ────────────────────────────────────────

bool FileManagerWindow::parse_ls_line(const std::string &line, FileEntry &entry) {
    // Expected format:
    // drwxr-xr-x  2 root root 4096 2024-01-15 10:30 dirname
    // lrwxrwxrwx  1 root root    6 2024-01-15 10:30 link -> target
    // -rw-r--r--  1 root root 1234 2024-01-15 10:30 file.txt

    if (line.empty() || line[0] == 't') return false; // "total N" line

    // Parse permissions (first field)
    std::istringstream iss(line);
    std::string perms_str;
    iss >> perms_str;
    if (perms_str.size() < 10) return false;
    if (perms_str[0] != 'd' && perms_str[0] != '-' && perms_str[0] != 'l') return false;

    entry.perms = perms_str;

    int links;
    iss >> links;
    entry.hard_links = links;

    std::string owner, group;
    iss >> owner >> group;
    entry.owner = owner;
    entry.group = group;

    long long sz;
    iss >> sz;
    entry.size = sz;

    // Date: YYYY-MM-DD HH:MM
    std::string date_part, time_part;
    iss >> date_part >> time_part;
    entry.date = date_part + " " + time_part;

    // Rest is the filename (may contain spaces, and symlink -> target)
    std::string rest;
    std::getline(iss, rest);
    // Trim leading spaces
    while (!rest.empty() && rest[0] == ' ') rest.erase(rest.begin());

    // Check for symlink
    auto arrow = rest.find(" -> ");
    if (arrow != std::string::npos) {
        entry.name = rest.substr(0, arrow);
        entry.link_target = rest.substr(arrow + 4);
    } else {
        entry.name = rest;
        entry.link_target = "";
    }

    // Skip . and .. entries
    if (entry.name == "." || entry.name == "..") return false;

    return !entry.name.empty();
}

// ── Navigation ───────────────────────────────────────────

void FileManagerWindow::navigate(const std::string &path) {
    if (g_busy) return;
    g_current_dir = path;
    SetWindowTextW(g_address_edit, utf8_to_wide(path).c_str());
    // Move cursor to end of text
    int len = GetWindowTextLengthW(g_address_edit);
    SendMessageW(g_address_edit, EM_SETSEL, len, len);
    refresh_list();
}

void FileManagerWindow::go_back() {
    if (g_current_dir == "/" || g_current_dir.empty()) return;

    // Go to parent directory
    std::string parent = g_current_dir;
    if (parent.back() == '/') parent.pop_back();
    auto last_sep = parent.find_last_of('/');
    if (last_sep == std::string::npos) {
        navigate("/");
    } else {
        navigate(parent.substr(0, last_sep + 1));
    }
}

void FileManagerWindow::enter_selected() {
    int sel = ListView_GetNextItem(g_file_list, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_entries.size()) return;

    const FileEntry &e = g_entries[sel];
    if (e.is_dir() || e.is_link()) {
        // For symlinks pointing to dirs, we just try to navigate
        navigate(build_remote_path(g_current_dir, e.name));
    }
}

// ── Refresh ──────────────────────────────────────────────

void FileManagerWindow::refresh_list() {
    if (!g_on_shell || g_busy) return;
    g_busy = true;
    if (g_on_busy) g_on_busy(true);
    if (g_status_label) SetWindowTextW(g_status_label, L"Loading...");
    DWORD tid;
    CreateThread(NULL, 0, LsThreadProc, NULL, 0, &tid);
}

DWORD WINAPI FileManagerWindow::LsThreadProc(LPVOID) {
    std::string cmd = "ls -al " + shell_escape(g_current_dir);
    std::string output;
    bool ok = g_on_shell(cmd, &output, 15);

    // Check for permission denied in output
    bool perm_denied = (output.find("Permission denied") != std::string::npos ||
                        output.find("permission denied") != std::string::npos ||
                        output.find("Operation not permitted") != std::string::npos);

    // Parse output
    std::vector<FileEntry> entries;
    if (ok && !perm_denied) {
        std::istringstream iss(output);
        std::string line;
        while (std::getline(iss, line)) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            FileEntry entry;
            if (parse_ls_line(line, entry)) {
                entries.push_back(std::move(entry));
            }
        }
    }

    // Sort: directories first, then files, alphabetically within each group
    std::sort(entries.begin(), entries.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.is_dir() != b.is_dir()) return a.is_dir();
        if (a.is_link() != b.is_link()) return a.is_link();
        return a.name < b.name;
    });

    g_entries = std::move(entries);

    // wp: 1=success, 2=permission denied, 0=other error
    int result = ok ? (perm_denied ? 2 : 1) : 0;
    PostMessageW(g_hwnd, WM_USER + 200, result, 0);
    return 0;
}

// ── Search ───────────────────────────────────────────────

void FileManagerWindow::search_files() {
    wchar_t wbuf[256];
    GetWindowTextW(g_search_edit, wbuf, 256);
    std::string query = wide_to_utf8(wbuf);
    if (query.empty()) { refresh_list(); return; }

    if (g_busy || !g_on_shell) return;
    g_busy = true;
    if (g_on_busy) g_on_busy(true);
    if (g_status_label) SetWindowTextW(g_status_label, L"Searching...");

    struct SearchData { std::string query; std::string dir; };
    auto *sdata = new SearchData{std::move(query), g_current_dir};

    DWORD tid;
    CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
        auto *sd = reinterpret_cast<SearchData*>(param);
        const std::string &query = sd->query;
        const std::string &dir = sd->dir;

        // Search strategy:
        // Write a shell script to device and execute it there.
        // This avoids the Windows command-line length limit (~8191 chars)
        // and allows using shell variables for compact HarmonyOS path enumeration.
        //
        // HarmonyOS key insight: /data has no read permission for shell user,
        // so find/glob can't enumerate subdirs. But shell has execute (x)
        // permission, so find with explicit paths works.
        // E.g. find /data fails, but find /data/local/tmp works (shell owns it).
        //
        // The script uses nested loops with known HarmonyOS directory names
        // to probe paths that can't be enumerated due to permission restrictions.

        // Build the search script content.
        // We write the script to a local temp file, push it to the device
        // via hdc file send, then execute it there. This avoids the Windows
        // command-line length limit (~8191 chars) and the heredoc-not-working
        // issue with hdc shell + CreateProcessA.
        // Build the search script content.
        // All find/for commands are wrapped in a subshell () so we can
        // pipe the output to sort -u | head -50 INSIDE the script.
        // This is critical because g_on_shell runs hdc.exe directly via
        // CreateProcessA (not cmd.exe), so pipe characters in the
        // shell command are NOT interpreted as shell pipes.
        std::string script;
        script += "#!/bin/sh\n";
        // Normalize DIR: strip trailing slash (except root "/")
        std::string norm_dir = dir;
        while (norm_dir.size() > 1 && norm_dir.back() == '/') norm_dir.pop_back();
        script += "DIR='" + norm_dir + "'\n";
        script += "Q='*" + query + "*'\n";
        script += "(\n";

        // When DIR is root "/", $DIR/subdir becomes "//subdir" which may fail.
        // Build a clean base: for root use empty string so /$d1 works correctly.
        if (dir == "/") {
            script += "find / -maxdepth 1 -name \"$Q\" 2>/dev/null\n";
            // Probe known HarmonyOS top-level paths with absolute paths
            // Note: /data has no read permission, so find /data fails.
            // We must probe /data/local, /data/local/tmp etc. with explicit paths.
            script += "for d1 in app service vendor log misc chipset system "
                      "storage update updater virt_service hmos4 system_ce module_update; do\n";
            script += "  test -d \"/$d1\" && find \"/$d1\" -name \"$Q\" 2>/dev/null\n";
            script += "done\n";
            // /data needs special handling — probe known subpaths explicitly
            script += "test -d /data/local && {\n";
            script += "  find /data/local -name \"$Q\" 2>/dev/null\n";
            script += "  test -d /data/local/tmp && find /data/local/tmp -name \"$Q\" 2>/dev/null\n";
            script += "}\n";
            script += "for d1 in local app data; do\n";
            script += "  for d2 in tmp traces mtp_tmp shader_cache el1 el2 el3 el4 el5 el0 "
                      "0 100 public bundle startup log camera gps bluetooth hiai audio_logs; do\n";
            script += "    test -d \"/data/$d1/$d2\" && find \"/data/$d1/$d2\" -name \"$Q\" 2>/dev/null\n";
            script += "  done\n";
            script += "done\n";
        } else {
            script += "find \"$DIR\" -name \"$Q\" 2>/dev/null\n";
            script += "find \"$DIR\"/* -name \"$Q\" 2>/dev/null\n";
            script += "find \"$DIR\"/*/* -name \"$Q\" 2>/dev/null\n";
            script += "find \"$DIR\"/*/*/* -name \"$Q\" 2>/dev/null\n";

            // HarmonyOS known paths — probe common subdirectory names at 3 levels
            script += "for d1 in local app service vendor log misc chipset system data "
                      "storage update updater virt_service hmos4 system_ce module_update; do\n";
            script += "  test -d \"$DIR/$d1\" && {\n";
            script += "    find \"$DIR/$d1\" -name \"$Q\" 2>/dev/null\n";
            script += "    for d2 in tmp traces mtp_tmp shader_cache "
                      "el1 el2 el3 el4 el5 el0 0 100 public bundle startup "
                      "log camera gps bluetooth hiai audio_logs; do\n";
            script += "      test -d \"$DIR/$d1/$d2\" && {\n";
            script += "        find \"$DIR/$d1/$d2\" -name \"$Q\" 2>/dev/null\n";
            script += "        for d3 in 0 100 base database sharefiles group "
                      "backup share hmdfs account huks_service public bundle startup; do\n";
            script += "          test -d \"$DIR/$d1/$d2/$d3\" && find \"$DIR/$d1/$d2/$d3\" -name \"$Q\" 2>/dev/null\n";
            script += "        done\n";
            script += "      }\n";
            script += "    done\n";
            script += "  }\n";
            script += "done\n";
        }

        script += ") | sort -u | head -50\n";
        const char *remote_script = "/data/local/tmp/_hscrcpy_search.sh";
        char local_tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, local_tmp);
        std::string local_script = std::string(local_tmp) + "_hscrcpy_search.sh";

        // Write local temp file
        FILE *fp = fopen(local_script.c_str(), "wb");
        if (fp) {
            fwrite(script.c_str(), 1, script.size(), fp);
            fclose(fp);

            // Push script to device
            if (g_on_push) {
                g_on_push(local_script, remote_script);
            }

            // Clean up local temp file
            _unlink(local_script.c_str());
        }

        // Execute the script on device
        std::string paths_output;
        g_on_shell("sh /data/local/tmp/_hscrcpy_search.sh", &paths_output, 30);

        // Clean up
        g_on_shell("rm -f /data/local/tmp/_hscrcpy_search.sh", nullptr, 5);

        // Parse found paths — normalize double slashes and deduplicate
        std::vector<std::string> paths;
        std::set<std::string> seen;
        std::istringstream piss(paths_output);
        std::string fpath;
        while (std::getline(piss, fpath)) {
            while (!fpath.empty() && fpath.back() == '\r') fpath.pop_back();
            if (fpath.empty()) continue;
            // Normalize: collapse // to /
            std::string norm;
            norm.reserve(fpath.size());
            for (size_t i = 0; i < fpath.size(); i++) {
                if (fpath[i] == '/' && !norm.empty() && norm.back() == '/') continue;
                norm.push_back(fpath[i]);
            }
            if (seen.insert(norm).second) paths.push_back(std::move(norm));
        }

        std::vector<FileEntry> entries;

        if (!paths.empty()) {
            // Phase 2: Get details for found paths using ls -ald
            // Batch paths to avoid command-line length limit (max ~50 paths per batch)
            const size_t batch_size = 30;
            for (size_t batch_start = 0; batch_start < paths.size(); batch_start += batch_size) {
                size_t batch_end = std::min(batch_start + batch_size, paths.size());
                std::string ls_cmd = "ls -ald";
                for (size_t i = batch_start; i < batch_end; ++i) {
                    ls_cmd += " " + shell_escape(paths[i]);
                }

                std::string ls_output;
                g_on_shell(ls_cmd, &ls_output, 15);

                std::istringstream lss(ls_output);
                std::string line;
                while (std::getline(lss, line)) {
                    while (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    FileEntry entry;
                    if (parse_ls_line(line, entry)) {
                        // entry.name from ls -ald is already the full path
                        // Skip duplicates by full path
                        bool dup = false;
                        for (auto &e : entries) {
                            if (e.name == entry.name) { dup = true; break; }
                        }
                        if (!dup) entries.push_back(std::move(entry));
                    }
                }
            }
        }

        delete sd;

        std::sort(entries.begin(), entries.end(), [](const FileEntry &a, const FileEntry &b) {
            if (a.is_dir() != b.is_dir()) return a.is_dir();
            return a.name < b.name;
        });

        g_entries = std::move(entries);
        PostMessageW(g_hwnd, WM_USER + 200, 1, 0);
        return 0;
    }, sdata, 0, &tid);
}

// ── File operations ──────────────────────────────────────

void FileManagerWindow::import_files() {
    if (!g_on_push || !g_on_shell) return;

    // Open file dialog (supports both files and folders via OFN_ALLOWMULTISELECT)
    OPENFILENAMEA ofn = {};
    char file_buf[4096] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = sizeof(file_buf);
    ofn.lpstrFilter = "All Files\0*.*\0\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    ofn.lpstrTitle = "Select files or folders to push to device";

    if (!GetOpenFileNameA(&ofn)) return;

    // Parse multi-select result
    std::string dir = file_buf;
    std::vector<std::string> files;
    char *p = file_buf + ofn.nFileOffset;
    if (*p == '\0') {
        // Single file
        files.push_back(dir);
    } else {
        // Multiple files
        while (*p) {
            files.push_back(dir + "\\" + p);
            p += strlen(p) + 1;
        }
    }

    if (files.empty()) return;

    g_busy = true;
    if (g_on_busy) g_on_busy(true);
    if (g_status_label) SetWindowTextW(g_status_label, L"Importing...");

    // Capture values for thread
    auto local_files = files;
    auto remote_dir = g_current_dir;

    DWORD tid;
    CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
        auto *data = reinterpret_cast<std::pair<std::vector<std::string>, std::string>*>(param);
        auto &local_files = data->first;
        auto &remote_dir = data->second;

        int ok = 0, fail = 0;
        std::string errors;
        for (auto &local : local_files) {
            // Extract filename from local path
            std::string name = local;
            auto bs = name.find_last_of("\\/");
            if (bs != std::string::npos) name = name.substr(bs + 1);

            std::string remote = ensure_dir_sep(remote_dir) + name;
            if (g_on_push(local, remote)) {
                g_on_shell("chmod 644 " + shell_escape(remote), nullptr, 5);
                ok++;
            } else {
                fail++;
                if (!errors.empty()) errors += "\n";
                errors += "Failed: " + name;
            }
        }

        delete data;

        static std::string s_last_error;
        s_last_error = errors;
        static int s_last_ok, s_last_fail;
        s_last_ok = ok;
        s_last_fail = fail;

        PostMessageW(g_hwnd, WM_USER + 201, s_last_ok, s_last_fail);
        return 0;
    }, new std::pair<std::vector<std::string>, std::string>(std::move(local_files), std::move(remote_dir)), 0, &tid);
}

void FileManagerWindow::export_files() {
    if (!g_on_pull) return;

    // Get selected entries
    std::vector<int> selected;
    int idx = -1;
    while ((idx = ListView_GetNextItem(g_file_list, idx, LVNI_SELECTED)) >= 0) {
        selected.push_back(idx);
    }
    if (selected.empty()) {
        msg_box("Export", "No files selected.");
        return;
    }

    // Check permissions for selected files
    for (int i : selected) {
        if (i >= (int)g_entries.size()) continue;
        if (!g_entries[i].can_read()) {
            msg_box("Permission Denied", "Cannot read '%s': %s\nNo read permission.",
                    g_entries[i].name.c_str(), g_entries[i].perms.c_str());
            return;
        }
    }

    // Select local destination folder
    char folder_buf[MAX_PATH] = {};
    BROWSEINFOA bi = {};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = "Select destination folder on PC";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return;
    SHGetPathFromIDListA(pidl, folder_buf);
    CoTaskMemFree(pidl);

    std::string local_dir = folder_buf;
    if (local_dir.empty()) return;

    g_busy = true;
    if (g_on_busy) g_on_busy(true);
    if (g_status_label) SetWindowTextW(g_status_label, L"Exporting...");

    // Capture values for thread
    std::vector<FileEntry> to_pull;
    for (int i : selected) {
        if (i < (int)g_entries.size()) to_pull.push_back(g_entries[i]);
    }
    auto remote_dir = g_current_dir;
    auto local_dir_val = local_dir;

    struct PullData {
        std::vector<FileEntry> entries;
        std::string remote_dir;
        std::string local_dir;
    };
    auto *pdata = new PullData{std::move(to_pull), std::move(remote_dir), std::move(local_dir_val)};

    DWORD tid;
    CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
        auto *data = reinterpret_cast<PullData*>(param);
        auto &entries = data->entries;
        auto &remote_dir = data->remote_dir;
        auto &local_dir = data->local_dir;

        int ok = 0, fail = 0;
        std::string errors;
        for (auto &e : entries) {
            if (e.is_dir()) {
                fail++;
                if (!errors.empty()) errors += "\n";
                errors += "Directory not supported: " + e.name;
                continue;
            }
            std::string remote = ensure_dir_sep(remote_dir) + e.name;
            std::string local = local_dir + "\\" + e.name;
            if (g_on_pull(remote, local)) {
                ok++;
            } else {
                fail++;
                if (!errors.empty()) errors += "\n";
                errors += "Failed: " + e.name;
            }
        }

        delete data;

        static std::string s_pull_error;
        s_pull_error = errors;
        static int s_pull_ok, s_pull_fail;
        s_pull_ok = ok;
        s_pull_fail = fail;

        PostMessageW(g_hwnd, WM_USER + 202, s_pull_ok, s_pull_fail);
        return 0;
    }, pdata, 0, &tid);
}

void FileManagerWindow::new_item() {
    if (!g_on_shell) return;

    // Ask: folder or file? — use a custom dialog with Folder/File buttons
    // 0 = cancelled, 1 = folder, 2 = file
    int choice = 0;
    {
        struct NewItemData { int *result; };
        NewItemData nd = { &choice };

        static bool reg_new = false;
        if (!reg_new) {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                auto *nd = reinterpret_cast<NewItemData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                switch (msg) {
                case WM_CREATE: {
                    auto *cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    nd = reinterpret_cast<NewItemData*>(cs->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)nd);

                    CreateWindowW(L"STATIC", L"Create new:",
                        WS_CHILD | WS_VISIBLE, 12, 14, 200, 18, hwnd, NULL, cs->hInstance, NULL);
                    CreateWindowW(L"BUTTON", L"Folder",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        30, 44, 100, 28, hwnd, (HMENU)1001, cs->hInstance, NULL);
                    CreateWindowW(L"BUTTON", L"File",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        150, 44, 100, 28, hwnd, (HMENU)1002, cs->hInstance, NULL);

                    HFONT hf = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                    SendMessageW(hwnd, WM_SETFONT, (WPARAM)hf, TRUE);
                    EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
                        SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE;
                    }, (LPARAM)hf);
                    return 0;
                }
                case WM_COMMAND:
                    if (LOWORD(wp) == 1001) { *nd->result = 1; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
                    else if (LOWORD(wp) == 1002) { *nd->result = 2; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
                    return 0;
                case WM_CLOSE:
                    DestroyWindow(hwnd);
                    return 0;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            };
            wc.hInstance = g_inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = L"HscrcpyNewItemDlg";
            RegisterClassExW(&wc);
            reg_new = true;
        }

        RECT orr;
        GetWindowRect(g_hwnd, &orr);
        HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
            L"HscrcpyNewItemDlg", L"New Item",
            WS_POPUP | WS_CAPTION,
            orr.left + (orr.right - orr.left - 280) / 2,
            orr.top + (orr.bottom - orr.top - 120) / 2,
            280, 120, g_hwnd, NULL, g_inst, &nd);

        EnableWindow(g_hwnd, FALSE);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        MSG m;
        while (IsWindow(dlg) && GetMessageW(&m, NULL, 0, 0)) {
            if (!IsWindow(dlg)) break;
            if (!IsDialogMessageW(dlg, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
        EnableWindow(g_hwnd, TRUE);
        SetFocus(g_hwnd);
    }

    if (choice == 0) return;  // cancelled
    bool is_folder = (choice == 1);

    std::wstring value = is_folder ? L"NewFolder" : L"newfile.txt";
    if (!show_input_dialog(is_folder ? L"New Folder" : L"New File",
                           is_folder ? L"Folder name:" : L"File name:", value)) return;
    std::string name = wide_to_utf8(value);

    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        msg_box("Invalid Name", "Name cannot contain / or \\");
        return;
    }

    std::string remote = ensure_dir_sep(g_current_dir) + name;
    std::string output;
    if (is_folder) {
        bool ret = g_on_shell("mkdir -p " + shell_escape(remote), &output, 10);
        if (!ret || shell_output_has_error(output)) {
            msg_box("Create Folder Failed", "Failed to create folder '%s'.\n%s\n\nCheck write permissions on the parent directory.",
                    name.c_str(), output.c_str());
            return;
        }
        g_on_shell("chmod 755 " + shell_escape(remote), nullptr, 5);
    } else {
        bool ret = g_on_shell("touch " + shell_escape(remote), &output, 10);
        if (!ret || shell_output_has_error(output)) {
            msg_box("Create File Failed", "Failed to create file '%s'.\n%s\n\nCheck write permissions on the parent directory.",
                    name.c_str(), output.c_str());
            return;
        }
        g_on_shell("chmod 644 " + shell_escape(remote), nullptr, 5);
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "Created %s", name.c_str());
    show_toast(buf, true);
    refresh_list();
}

void FileManagerWindow::delete_selected() {
    if (!g_on_shell) return;

    std::vector<int> selected;
    int idx = -1;
    while ((idx = ListView_GetNextItem(g_file_list, idx, LVNI_SELECTED)) >= 0) {
        selected.push_back(idx);
    }
    if (selected.empty()) return;

    // Build confirmation message
    std::string names;
    int dir_count = 0, file_count = 0;
    for (int i : selected) {
        if (i >= (int)g_entries.size()) continue;
        if (!names.empty() && names.size() < 200) names += ", ";
        if (names.size() < 200) names += g_entries[i].name;
        if (g_entries[i].is_dir()) dir_count++;
        else file_count++;
    }
    if (names.size() >= 200) names += "...";

    char msg[512];
    snprintf(msg, sizeof(msg), "Delete %d item(s)?\n\n%s\n\nThis action cannot be undone.",
             (int)selected.size(), names.c_str());
    if (!confirm("Confirm Delete", "%s", msg)) return;

    // Check write permissions
    for (int i : selected) {
        if (i >= (int)g_entries.size()) continue;
        // Check parent directory write permission (we need write on parent to delete)
        // We can't easily check parent perms from current listing, so just try and report errors
    }

    g_busy = true;
    if (g_on_busy) g_on_busy(true);
    if (g_status_label) SetWindowTextW(g_status_label, L"Deleting...");

    std::vector<FileEntry> to_delete;
    for (int i : selected) {
        if (i < (int)g_entries.size()) to_delete.push_back(g_entries[i]);
    }
    auto remote_dir = g_current_dir;

    DWORD tid;
    CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
        auto *data = reinterpret_cast<std::pair<std::vector<FileEntry>, std::string>*>(param);
        auto &entries = data->first;
        auto &dir = data->second;

        int ok = 0, fail = 0;
        std::string errors;
        for (auto &e : entries) {
            std::string remote = ensure_dir_sep(dir) + e.name;
            std::string cmd = e.is_dir() ? "rm -rf " + shell_escape(remote) : "rm " + shell_escape(remote);
            std::string output;
            bool ret = g_on_shell(cmd, &output, 30);
            if (ret && !shell_output_has_error(output)) {
                ok++;
            } else {
                fail++;
                if (!errors.empty()) errors += "\n";
                errors += "Failed: " + e.name;
                if (output.find("Permission denied") != std::string::npos ||
                    output.find("permission denied") != std::string::npos) {
                    errors += " (Permission denied)";
                } else if (output.find("Read-only") != std::string::npos) {
                    errors += " (Read-only filesystem)";
                }
            }
        }

        delete data;

        static std::string s_del_error;
        s_del_error = errors;
        static int s_del_ok, s_del_fail;
        s_del_ok = ok;
        s_del_fail = fail;

        PostMessageW(g_hwnd, WM_USER + 203, s_del_ok, s_del_fail);
        return 0;
    }, new std::pair<std::vector<FileEntry>, std::string>(std::move(to_delete), std::move(remote_dir)), 0, &tid);
}

void FileManagerWindow::copy_selected() {
    std::vector<int> selected;
    int idx = -1;
    while ((idx = ListView_GetNextItem(g_file_list, idx, LVNI_SELECTED)) >= 0) {
        selected.push_back(idx);
    }
    if (selected.empty()) return;

    g_clipboard.entries.clear();
    g_clipboard.source_dir = g_current_dir;
    g_clipboard.action = FileClipAction::Copy;
    for (int i : selected) {
        if (i < (int)g_entries.size()) g_clipboard.entries.push_back(g_entries[i]);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "Copied %d item(s)", (int)g_clipboard.entries.size());
    show_toast(buf, true);
    update_status();
}

void FileManagerWindow::paste_clipboard() {
    if (!g_on_shell || g_clipboard.entries.empty()) return;

    // Check if pasting into same directory for copy
    if (g_clipboard.action == FileClipAction::Copy && g_clipboard.source_dir == g_current_dir) {
        msg_box("Paste", "Cannot paste into the same directory.\nNavigate to a different directory first.");
        return;
    }

    g_busy = true;
    if (g_on_busy) g_on_busy(true);
    if (g_status_label) SetWindowTextW(g_status_label, L"Pasting...");

    auto clipboard_copy = g_clipboard;
    clipboard_copy.dest_dir = g_current_dir;

    DWORD tid;
    CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
        auto *cb = reinterpret_cast<FileClipboard*>(param);
        int ok = 0, fail = 0;
        std::string errors;

        for (auto &e : cb->entries) {
            std::string src = ensure_dir_sep(cb->source_dir) + e.name;
            std::string dst = ensure_dir_sep(cb->dest_dir) + e.name;

            std::string cmd;
            if (cb->action == FileClipAction::Move) {
                cmd = "mv " + shell_escape(src) + " " + shell_escape(dst);
            } else {
                cmd = e.is_dir() ? "cp -r " + shell_escape(src) + " " + shell_escape(dst) : "cp " + shell_escape(src) + " " + shell_escape(dst);
            }

            std::string output;
            bool ret = g_on_shell(cmd, &output, 30);
            if (ret && !shell_output_has_error(output)) {
                ok++;
            } else {
                fail++;
                if (!errors.empty()) errors += "\n";
                errors += "Failed: " + e.name;
                if (output.find("Permission denied") != std::string::npos ||
                    output.find("permission denied") != std::string::npos) {
                    errors += " (Permission denied)";
                } else if (output.find("Read-only") != std::string::npos) {
                    errors += " (Read-only filesystem)";
                }
            }
        }

        // Clear clipboard after move
        if (cb->action == FileClipAction::Move) {
            g_clipboard.entries.clear();
            g_clipboard.action = FileClipAction::None;
        }

        delete cb;

        static std::string s_paste_error;
        s_paste_error = errors;
        static int s_paste_ok, s_paste_fail;
        s_paste_ok = ok;
        s_paste_fail = fail;

        PostMessageW(g_hwnd, WM_USER + 204, s_paste_ok, s_paste_fail);
        return 0;
    }, new FileClipboard(clipboard_copy), 0, &tid);
}

void FileManagerWindow::rename_selected() {
    if (!g_on_shell) return;

    int sel = ListView_GetNextItem(g_file_list, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_entries.size()) return;

    const FileEntry &e = g_entries[sel];

    // Check write permission on the file
    if (!e.can_write()) {
        msg_box("Permission Denied", "Cannot rename '%s': no write permission (%s).\n\nOwner: %s  Group: %s",
                e.name.c_str(), e.perms.c_str(), e.owner.c_str(), e.group.c_str());
        return;
    }

    // Input dialog for new name
    std::wstring value = utf8_to_wide(e.name);
    if (!show_input_dialog(L"Rename", L"New name:", value)) return;
    std::string new_name = wide_to_utf8(value);

    if (new_name == e.name) return;
    if (new_name.find('/') != std::string::npos || new_name.find('\\') != std::string::npos) {
        msg_box("Invalid Name", "Name cannot contain / or \\");
        return;
    }

    std::string old_path = ensure_dir_sep(g_current_dir) + e.name;
    std::string new_path = ensure_dir_sep(g_current_dir) + new_name;

    std::string output;
    bool ret = g_on_shell("mv " + shell_escape(old_path) + " " + shell_escape(new_path), &output, 10);
    if (!ret || shell_output_has_error(output)) {
        msg_box("Rename Failed", "Failed to rename '%s' to '%s'.\n%s",
                e.name.c_str(), new_name.c_str(), output.c_str());
        return;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "Renamed to %s", new_name.c_str());
    show_toast(buf, true);
    refresh_list();
}

// ── Context menu ─────────────────────────────────────────

void FileManagerWindow::show_context_menu(POINT pt) {
    HMENU hMenu = CreatePopupMenu();

    int sel = ListView_GetNextItem(g_file_list, -1, LVNI_SELECTED);
    bool has_sel = (sel >= 0);

    if (has_sel && sel < (int)g_entries.size()) {
        const FileEntry &e = g_entries[sel];
        if (e.is_dir()) AppendMenuW(hMenu, MF_STRING, IDM_OPEN, L"Open");
        AppendMenuW(hMenu, MF_STRING, IDM_PULL, L"Export to PC...");        AppendMenuW(hMenu, MF_STRING, IDM_COPY, L"Copy");
        AppendMenuW(hMenu, MF_STRING, IDM_RENAME, L"Rename");
        AppendMenuW(hMenu, MF_STRING, IDM_DELETE, L"Delete");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, IDM_PROPERTIES, L"Properties");
    } else {
        AppendMenuW(hMenu, MF_STRING, IDM_PUSH, L"Import from PC...");
        AppendMenuW(hMenu, MF_STRING, IDM_NEWFOLDER, L"New Item...");
        if (!g_clipboard.entries.empty()) {
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, IDM_PASTE, L"Paste");
            if (g_clipboard.action == FileClipAction::Move) {
                AppendMenuW(hMenu, MF_STRING, IDM_MOVE_HERE, L"Move Here");
            }
        }
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, IDM_REFRESH, L"Refresh");
    }

    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(hMenu);
}

// ── Properties dialog ────────────────────────────────────

void FileManagerWindow::show_properties() {
    int sel = ListView_GetNextItem(g_file_list, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_entries.size()) return;

    const FileEntry &e = g_entries[sel];
    std::string path = ensure_dir_sep(g_current_dir) + e.name;

    // Get detailed stat info
    std::string stat_output;
    if (g_on_shell) {
        g_on_shell("stat " + shell_escape(path), &stat_output, 5);
    }

    std::string info;
    info += "Name: " + e.name + "\n";
    info += "Path: " + path + "\n";
    info += "Type: " + std::string(e.is_dir() ? "Directory" : e.is_link() ? "Symbolic Link" : "File") + "\n";
    info += "Permissions: " + e.perms + "\n";
    info += "Owner: " + e.owner + "\n";
    info += "Group: " + e.group + "\n";
    if (!e.is_dir()) info += "Size: " + format_size(e.size) + "\n";
    info += "Modified: " + e.date + "\n";
    if (e.is_link()) info += "Target: " + e.link_target + "\n";
    if (!stat_output.empty()) info += "\n--- stat ---\n" + stat_output;

    MessageBoxW(g_hwnd, utf8_to_wide(info).c_str(), utf8_to_wide("Properties - " + e.name).c_str(), MB_OK | MB_ICONINFORMATION);
}

// ── Status / messages ────────────────────────────────────

void FileManagerWindow::update_status() {
    if (!g_status_label) return;

    int count = (int)g_entries.size();
    int sel_count = ListView_GetSelectedCount(g_file_list);

    std::string status = std::to_string(count) + " items";
    if (sel_count > 0) status += ", " + std::to_string(sel_count) + " selected";
    if (!g_clipboard.entries.empty()) {
        status += " | Clipboard: " + std::to_string(g_clipboard.entries.size());
        status += (g_clipboard.action == FileClipAction::Copy ? " (copy)" : " (move)");
    }
    SetWindowTextW(g_status_label, utf8_to_wide(status).c_str());
}

void FileManagerWindow::msg_box(const char *title, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    MessageBoxW(g_hwnd, utf8_to_wide(buf).c_str(), utf8_to_wide(title).c_str(), MB_OK | MB_ICONWARNING);
}

bool FileManagerWindow::confirm(const char *title, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return MessageBoxW(g_hwnd, utf8_to_wide(buf).c_str(), utf8_to_wide(title).c_str(),
                       MB_YESNO | MB_ICONQUESTION) == IDYES;
}

// ── Toast notification ───────────────────────────────────

#define ID_TOAST_TIMER 9999
#define TOAST_DURATION 2500  // ms

LRESULT CALLBACK FileManagerWindow::ToastWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Auto-dismiss timer
        SetTimer(hwnd, ID_TOAST_TIMER, TOAST_DURATION, NULL);
        break;
    }
    case WM_TIMER:
        if (wp == ID_TOAST_TIMER) {
            KillTimer(hwnd, ID_TOAST_TIMER);
            // Fade out by animating window position downward, then destroy
            AnimateWindow(hwnd, 200, AW_HIDE | AW_SLIDE | AW_VER_POSITIVE);
            DestroyWindow(hwnd);
            FileManagerWindow::g_toast_hwnd = nullptr;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Background color stored as window data
        bool success = (GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0);
        HBRUSH bg = CreateSolidBrush(RGB(50, 50, 50));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        // Text
        wchar_t text[512] = {};
        GetWindowTextW(hwnd, text, 512);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, success ? RGB(220, 220, 220) : RGB(255, 120, 120));
        HFONT hf = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(hdc, hf);
        RECT trc = rc;
        trc.left += 12;
        trc.right -= 12;
        DrawTextW(hdc, text, -1, &trc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, old);
        DeleteObject(hf);

        EndPaint(hwnd, &ps);
        break;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONUP:
        // Click to dismiss
        KillTimer(hwnd, ID_TOAST_TIMER);
        DestroyWindow(hwnd);
        FileManagerWindow::g_toast_hwnd = nullptr;
        break;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

void FileManagerWindow::show_toast(const char *text, bool success) {
    if (!g_hwnd) return;

    // Register toast window class once
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = ToastWndProc;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"HscrcpyToast";
        RegisterClassW(&wc);
        registered = true;
    }

    // Destroy previous toast if still visible
    if (g_toast_hwnd) {
        KillTimer(g_toast_hwnd, ID_TOAST_TIMER);
        DestroyWindow(g_toast_hwnd);
        g_toast_hwnd = nullptr;
    }

    // Measure text width
    HDC hdc = GetDC(g_hwnd);
    HFONT hf = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, hf);
    std::wstring wtext = utf8_to_wide(text);
    SIZE sz;
    GetTextExtentPoint32W(hdc, wtext.c_str(), (int)wtext.size(), &sz);
    SelectObject(hdc, old);
    DeleteObject(hf);
    ReleaseDC(g_hwnd, hdc);

    int tw = sz.cx + 28;
    int th = 32;
    RECT parent_rc;
    GetClientRect(g_hwnd, &parent_rc);
    int x = (parent_rc.right - tw) / 2;
    int y = (parent_rc.bottom - th) / 2;
    POINT pt = {x, y};
    ClientToScreen(g_hwnd, &pt);
    x = pt.x;
    y = pt.y;

    g_toast_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                    L"HscrcpyToast", wtext.c_str(),
                                    WS_POPUP | WS_CLIPCHILDREN,
                                    x, y, tw, th,
                                    g_hwnd, NULL, g_inst, NULL);
    if (g_toast_hwnd) {
        SetWindowLongPtrW(g_toast_hwnd, GWLP_USERDATA, success ? 1 : 0);
        // Semi-transparent
        SetLayeredWindowAttributes(g_toast_hwnd, 0, 220, LWA_ALPHA);
        AnimateWindow(g_toast_hwnd, 150, AW_SLIDE | AW_VER_NEGATIVE);
    }
}

std::string FileManagerWindow::build_remote_path(const std::string &dir, const std::string &name) {
    if (dir == "/") return "/" + name;
    return ensure_dir_sep(dir) + name;
}

// ── Populate ListView ────────────────────────────────────

void FileManagerWindow::populate_list() {
    ListView_DeleteAllItems(g_file_list);

    for (int i = 0; i < (int)g_entries.size(); i++) {
        const FileEntry &e = g_entries[i];

        // Icon index
        int icon_idx = 2; // default: link
        if (e.is_dir()) icon_idx = 0;
        else if (e.is_file()) icon_idx = 1;

        std::wstring wname = utf8_to_wide(e.name);
        // In root directory, prefix subdirectories with '/' for clarity
        if (g_current_dir == "/" && e.is_dir() && e.name.find('/') == std::string::npos)
            wname = L"/" + wname;
        if (e.is_dir()) wname += L"/";
        if (e.is_link()) wname += L" -> " + utf8_to_wide(e.link_target);

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT | LVIF_IMAGE;
        lvi.iItem = i;
        lvi.pszText = const_cast<LPWSTR>(wname.c_str());
        lvi.iImage = icon_idx;
        int idx = (int)SendMessageW(g_file_list, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

        // Sub-items: Permissions, Owner, Group, Size, Date
        std::wstring wperms = utf8_to_wide(e.perms);
        std::wstring wowner = utf8_to_wide(e.owner);
        std::wstring wgroup = utf8_to_wide(e.group);
        std::wstring wsize = utf8_to_wide(e.is_dir() ? "<DIR>" : format_size(e.size));
        std::wstring wdate = utf8_to_wide(e.date);

        LVITEMW sub = {};
        sub.mask = LVIF_TEXT;
        sub.iItem = idx;

        sub.iSubItem = 1; sub.pszText = const_cast<LPWSTR>(wperms.c_str());
        SendMessageW(g_file_list, LVM_SETITEMTEXTW, idx, (LPARAM)&sub);
        sub.iSubItem = 2; sub.pszText = const_cast<LPWSTR>(wowner.c_str());
        SendMessageW(g_file_list, LVM_SETITEMTEXTW, idx, (LPARAM)&sub);
        sub.iSubItem = 3; sub.pszText = const_cast<LPWSTR>(wgroup.c_str());
        SendMessageW(g_file_list, LVM_SETITEMTEXTW, idx, (LPARAM)&sub);
        sub.iSubItem = 4; sub.pszText = const_cast<LPWSTR>(wsize.c_str());
        SendMessageW(g_file_list, LVM_SETITEMTEXTW, idx, (LPARAM)&sub);
        sub.iSubItem = 5; sub.pszText = const_cast<LPWSTR>(wdate.c_str());
        SendMessageW(g_file_list, LVM_SETITEMTEXTW, idx, (LPARAM)&sub);
    }
}

// ── WndProc ──────────────────────────────────────────────

LRESULT CALLBACK FileManagerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // File-list icons: use ImageList_AddMasked with magenta background.
        // Color bitmap has solid tint where alpha>0, magenta where alpha=0.
        // ImageList_AddMasked auto-generates the mask from the transparent color.
        g_img_list = ImageList_Create(24, 24, ILC_COLOR32 | ILC_MASK, 3, 0);

        auto add_il_icon = [&](const char *path, BYTE r, BYTE g, BYTE b) {
            char exe_path[MAX_PATH];
            GetModuleFileNameA(NULL, exe_path, MAX_PATH);
            std::string dir(exe_path);
            auto sep = dir.find_last_of("\\/");
            if (sep != std::string::npos) dir = dir.substr(0, sep + 1);
            std::string fpath = dir + path;
            int wlen = MultiByteToWideChar(CP_ACP, 0, fpath.c_str(), -1, NULL, 0);
            std::wstring wpath(wlen - 1, L'\0');
            MultiByteToWideChar(CP_ACP, 0, fpath.c_str(), -1, &wpath[0], wlen);

            Gdiplus::Bitmap *src = Gdiplus::Bitmap::FromFile(wpath.c_str(), FALSE);
            if (!src || src->GetLastStatus() != Gdiplus::Ok) {
                delete src;
                ImageList_AddIcon(g_img_list, LoadIcon(NULL, IDI_WINLOGO));
                return;
            }

            // Scale source
            Gdiplus::Bitmap *scaled = new Gdiplus::Bitmap(24, 24, PixelFormat32bppARGB);
            { Gdiplus::Graphics gs(scaled);
              gs.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
              gs.Clear(Gdiplus::Color(0, 0, 0, 0));
              gs.DrawImage(src, 0, 0, 24, 24); }
            delete src;

            // Read alpha channel
            Gdiplus::BitmapData bd = {};
            Gdiplus::Rect rect(0, 0, 24, 24);
            scaled->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd);

            // Create color DIB: tint where alpha>0, magenta where alpha=0
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = 24;
            bmi.bmiHeader.biHeight = -24;  // top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            BYTE *color_bits = NULL;
            HBITMAP hbmColor = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void **)&color_bits, NULL, 0);

            if (hbmColor && color_bits) {
                for (int y = 0; y < 24; y++) {
                    const BYTE *sp = (const BYTE *)bd.Scan0 + y * bd.Stride;
                    BYTE *dp = color_bits + y * 24 * 4;
                    for (int x = 0; x < 24; x++) {
                        BYTE a = sp[3];
                        if (a > 0) {
                            dp[0] = b; dp[1] = g; dp[2] = r; dp[3] = 0;
                        } else {
                            // Magenta = transparent marker for AddMasked
                            dp[0] = 255; dp[1] = 0; dp[2] = 255; dp[3] = 0;
                        }
                        sp += 4; dp += 4;
                    }
                }
            }

            scaled->UnlockBits(&bd);
            delete scaled;

            if (hbmColor) {
                ImageList_AddMasked(g_img_list, hbmColor, RGB(255, 0, 255));
                DeleteObject(hbmColor);
            } else {
                ImageList_AddIcon(g_img_list, LoadIcon(NULL, IDI_WINLOGO));
            }
        };
        add_il_icon("res\\file-manager\\folder.png", 85, 85, 85);
        add_il_icon("res\\file-manager\\file.png", 85, 85, 85);
        add_il_icon("res\\file-manager\\file-link.png", 85, 85, 85);

        HFONT hf = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        // Row 1: Folder icon + Address bar + Go + Back + Search edit + Search btn
        int y1 = MARGIN;
        int cx = MARGIN;

        // Static folder icon (decoration, not clickable)
        g_folder_icon = CreateWindowW(L"STATIC", L"",
                                        WS_CHILD | WS_VISIBLE | SS_ICON,
                                        cx, y1, BTN_SIZE, BTN_SIZE, hwnd, NULL, g_inst, NULL);
        HICON hFolder = load_icon_file("res\\file-manager\\folder.png", 24, 85, 85, 85, true);
        if (hFolder) SendMessageW(g_folder_icon, STM_SETICON, (WPARAM)hFolder, 0);
        cx += BTN_SIZE + 2;

        // Address bar, search edit — widths will be set in WM_SIZE
        // Edit height 22 (text height), y+2 to vertically center in 26px row
        int edit_h = 22;
        int edit_y = y1 + (BTN_SIZE - edit_h) / 2;
        g_address_edit = CreateWindowExW(0, L"EDIT", L"/",
                                           WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                           cx, edit_y, 200, edit_h, hwnd, (HMENU)IDC_ADDRESS, g_inst, NULL);
        SendMessageW(g_address_edit, WM_SETFONT, (WPARAM)hf, TRUE);

        g_go_btn = CreateWindowW(L"BUTTON", L"",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_ICON,
                                   cx + 202, y1, BTN_SIZE, BTN_SIZE, hwnd, (HMENU)IDC_GO_BTN, g_inst, NULL);
        HICON hGo = load_icon_file("res\\file-manager\\go.png", 24, 85, 85, 85, true);
        if (hGo) { SendMessageW(g_go_btn, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hGo); }
        else SetWindowTextW(g_go_btn, L"Go");

        g_back_btn = CreateWindowW(L"BUTTON", L"",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_ICON,
                                     cx + 232, y1, BTN_SIZE, BTN_SIZE, hwnd, (HMENU)IDC_BACK_BTN, g_inst, NULL);
        HICON hBack = load_icon_file("res\\file-manager\\nav-back.png", 24, 85, 85, 85, true);
        if (hBack) { SendMessageW(g_back_btn, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hBack); }
        else SetWindowTextW(g_back_btn, L"\u2190");

        g_search_edit = CreateWindowExW(0, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                          cx + 260, edit_y, 100, edit_h, hwnd, (HMENU)IDC_SEARCH_EDIT, g_inst, NULL);
        SendMessageW(g_search_edit, WM_SETFONT, (WPARAM)hf, TRUE);

        // Subclass edit controls to handle Enter key
        g_addr_orig_proc = (WNDPROC)SetWindowLongPtrW(g_address_edit, GWLP_WNDPROC, (LONG_PTR)AddrEditProc);
        g_search_orig_proc = (WNDPROC)SetWindowLongPtrW(g_search_edit, GWLP_WNDPROC, (LONG_PTR)SearchEditProc);

        g_search_btn = CreateWindowW(L"BUTTON", L"",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_ICON,
                                       cx + 362, y1, BTN_SIZE, BTN_SIZE, hwnd, (HMENU)IDC_SEARCH_BTN, g_inst, NULL);
        HICON hSearch = load_icon_file("res\\file-manager\\folder-search.png", 24, 85, 85, 85, true);
        if (hSearch) { SendMessageW(g_search_btn, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hSearch); }
        else SetWindowTextW(g_search_btn, L"\U0001F50D");

        // Row 2: Toolbar buttons with icons
        int y2 = y1 + BTN_SIZE + 4;
        int bx = MARGIN;

        auto make_icon_btn = [&](CtrlId id, const char *icon_path, const wchar_t *fallback) -> HWND {
            HWND btn = CreateWindowW(L"BUTTON", L"",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_ICON,
                                      bx, y2, BTN_SIZE, BTN_SIZE, hwnd, (HMENU)id, g_inst, NULL);
            HICON hIco = load_icon_file(icon_path, 24, 85, 85, 85, true);
            if (hIco) { SendMessageW(btn, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hIco); }
            else SetWindowTextW(btn, fallback);
            bx += BTN_SIZE + 2;
            return btn;
        };

        g_import_btn = make_icon_btn(IDC_IMPORT_BTN, "res\\file-manager\\file-import.png", L"Push");
        g_export_btn = make_icon_btn(IDC_EXPORT_BTN, "res\\file-manager\\file-export.png", L"Pull");
        g_copy_btn = make_icon_btn(IDC_COPY_BTN, "res\\file-manager\\file-copy.png", L"Cp");
        g_paste_btn = make_icon_btn(IDC_PASTE_BTN, "res\\file-manager\\file-paste.png", L"Pst");
        g_rename_btn = make_icon_btn(IDC_RENAME_BTN, "res\\file-manager\\file-rename.png", L"Rn");
        g_new_btn = make_icon_btn(IDC_NEW_BTN, "res\\file-manager\\file-add.png", L"New");
        g_delete_btn = make_icon_btn(IDC_DELETE_BTN, "res\\file-manager\\file-delete.png", L"Del");

        // Remove visual styles from buttons so WM_CTLCOLORBTN works for white background
        for (HWND btn : {g_back_btn, g_go_btn, g_search_btn,
                         g_import_btn, g_export_btn, g_new_btn, g_delete_btn,
                         g_copy_btn, g_paste_btn, g_rename_btn}) {
            SetWindowTheme(btn, L"", L"");
        }

        // Row 3: File list (ListView)
        int y3 = y2 + 26;
        g_file_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                        WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS,
                                        MARGIN, y3, 100, 100, hwnd, (HMENU)IDC_FILE_LIST, g_inst, NULL);
        SendMessageW(g_file_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                     LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        SendMessageW(g_file_list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)g_img_list);
        SendMessageW(g_file_list, WM_SETFONT, (WPARAM)hf, TRUE);

        // Columns: Name, Permissions, Owner, Group, Size, Date
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = LVCFMT_LEFT;
        int widths[] = {160, 110, 60, 60, 70, 110};
        const wchar_t *headers[] = {L"Name", L"Permissions", L"Owner", L"Group", L"Size", L"Date"};
        for (int i = 0; i < 6; i++) {
            col.cx = widths[i];
            col.pszText = const_cast<LPWSTR>(headers[i]);
            SendMessageW(g_file_list, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
        }

        // Status label
        g_status_label = CreateWindowW(L"STATIC", L"File Manager",
                                         WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         MARGIN, 1, 200, 18, hwnd, (HMENU)IDC_STATUS_LABEL, g_inst, NULL);
        SendMessageW(g_status_label, WM_SETFONT, (WPARAM)hf, TRUE);

        break;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        int content_w = w - MARGIN * 2;

        int y1 = MARGIN;
        int y2 = y1 + BTN_SIZE + 4;
        int y3 = y2 + BTN_SIZE + 6;
        int status_h = 20;
        int list_h = h - y3 - status_h - MARGIN;
        if (list_h < 40) list_h = 40;

        // Reposition row 1: Folder icon + Address + Go + Back + Search edit + Search btn
        // Layout: [📁 26] [Address ~60%] [Go 28] [← 26] [Search ~30%] [🔎 26]
        int folder_w = BTN_SIZE;
        int go_w = BTN_SIZE, back_w = BTN_SIZE, search_btn_w = BTN_SIZE;
        int fixed_w = folder_w + go_w + back_w + search_btn_w + 8; // 4 gaps
        int remaining = content_w - fixed_w;
        int addr_w = (int)(remaining * 0.6);
        int search_w = remaining - addr_w;
        if (addr_w < 80) addr_w = 80;
        if (search_w < 50) search_w = 50;

        int cx = MARGIN;
        int edit_h = 22;
        int edit_y = y1 + (BTN_SIZE - edit_h) / 2;
        MoveWindow(g_folder_icon, cx, y1, folder_w, BTN_SIZE, FALSE);
        cx += folder_w + 2;
        MoveWindow(g_address_edit, cx, edit_y, addr_w, edit_h, FALSE);
        cx += addr_w + 2;
        MoveWindow(g_go_btn, cx, y1, go_w, BTN_SIZE, FALSE);
        cx += go_w + 2;
        MoveWindow(g_back_btn, cx, y1, back_w, BTN_SIZE, FALSE);
        cx += back_w + 2;
        MoveWindow(g_search_edit, cx, edit_y, search_w, edit_h, FALSE);
        cx += search_w + 2;
        MoveWindow(g_search_btn, cx, y1, search_btn_w, BTN_SIZE, FALSE);

        // Toolbar buttons stay same position
        MoveWindow(g_file_list, MARGIN, y3, content_w, list_h, FALSE);
        MoveWindow(g_status_label, MARGIN, y3 + list_h + MARGIN, content_w, 18, FALSE);

        // Auto-size columns
        if (content_w > 0) {
            int name_w = content_w - 110 - 60 - 60 - 70 - 110;
            if (name_w < 80) name_w = 80;
            SendMessageW(g_file_list, LVM_SETCOLUMNWIDTH, 0, name_w);
            SendMessageW(g_file_list, LVM_SETCOLUMNWIDTH, 1, 110);
            SendMessageW(g_file_list, LVM_SETCOLUMNWIDTH, 2, 60);
            SendMessageW(g_file_list, LVM_SETCOLUMNWIDTH, 3, 60);
            SendMessageW(g_file_list, LVM_SETCOLUMNWIDTH, 4, 70);
            SendMessageW(g_file_list, LVM_SETCOLUMNWIDTH, 5, (LPARAM)-2); // auto-fill
        }

        RedrawWindow(hwnd, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        break;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, RGB(255, 255, 255));
        SetTextColor(hdc, RGB(0, 0, 0));
        static HBRUSH white_brush = CreateSolidBrush(RGB(255, 255, 255));
        return (LRESULT)white_brush;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
        return 1;
    }
    case WM_NOTIFY: {
        NMHDR *nm = reinterpret_cast<NMHDR *>(lp);
        if (nm->idFrom == IDC_FILE_LIST) {
            if (nm->code == NM_DBLCLK) {
                auto *nmitem = reinterpret_cast<NMITEMACTIVATE *>(lp);
                if (nmitem && nmitem->iItem >= 0) {
                    enter_selected();
                }
            } else if (nm->code == NM_RCLICK) {
                POINT pt;
                GetCursorPos(&pt);
                show_context_menu(pt);
            } else if (nm->code == LVN_KEYDOWN) {
                auto *lvkd = reinterpret_cast<NMLVKEYDOWN *>(lp);
                if (lvkd->wVKey == VK_DELETE) {
                    delete_selected();
                } else if (lvkd->wVKey == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    copy_selected();
                } else if (lvkd->wVKey == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    paste_clipboard();
                } else if (lvkd->wVKey == VK_RETURN) {
                    enter_selected();
                } else if (lvkd->wVKey == VK_BACK) {
                    go_back();
                } else if (lvkd->wVKey == VK_F5) {
                    refresh_list();
                }
            } else if (nm->code == LVN_ITEMCHANGED) {
                update_status();
            }
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_GO_BTN: {
            wchar_t wbuf[512];
            GetWindowTextW(g_address_edit, wbuf, 512);
            navigate(wide_to_utf8(wbuf));
            break;
        }
        case IDC_UP_BTN: break; // removed
        case IDC_BACK_BTN: go_back(); break;
        case IDC_SEARCH_BTN: search_files(); break;
        case IDC_IMPORT_BTN: import_files(); break;
        case IDC_EXPORT_BTN: export_files(); break;
        case IDC_NEW_BTN: new_item(); break;
        case IDC_DELETE_BTN: delete_selected(); break;
        case IDC_COPY_BTN: copy_selected(); break;
        case IDC_PASTE_BTN: paste_clipboard(); break;
        case IDC_RENAME_BTN: rename_selected(); break;
        // Context menu commands
        case IDM_OPEN: enter_selected(); break;
        case IDM_PUSH: import_files(); break;
        case IDM_PULL: export_files(); break;
        case IDM_COPY: copy_selected(); break;
        case IDM_PASTE: paste_clipboard(); break;
        case IDM_MOVE_HERE: {
            g_clipboard.action = FileClipAction::Move;
            paste_clipboard();
            break;
        }
        case IDM_RENAME: rename_selected(); break;
        case IDM_DELETE: delete_selected(); break;
        case IDM_NEWFOLDER: new_item(); break;
        case IDM_REFRESH: refresh_list(); break;
        case IDM_PROPERTIES: show_properties(); break;
        }
        break;

    // ls result
    case WM_USER + 200: {
        g_busy = false;
        if (g_on_busy) g_on_busy(false);
        if (wp == 1) {
            populate_list();
            update_status();
        } else if (wp == 2) {
            // Permission denied
            ListView_DeleteAllItems(g_file_list);
            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT;
            static const wchar_t *s_no_perm = L"No Permission";
            lvi.pszText = const_cast<LPWSTR>(s_no_perm);
            int idx = (int)SendMessageW(g_file_list, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
            if (idx >= 0) {
                LVITEMW sub = {};
                sub.mask = LVIF_TEXT;
                sub.iItem = idx;
                sub.iSubItem = 1;
                static const wchar_t *s_access_denied = L"Access denied to this directory";
                sub.pszText = const_cast<LPWSTR>(s_access_denied);
                SendMessageW(g_file_list, LVM_SETITEMTEXTW, idx, (LPARAM)&sub);
            }
            if (g_status_label) SetWindowTextW(g_status_label, L"No Permission");
        } else {
            ListView_DeleteAllItems(g_file_list);
            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT;
            static const wchar_t *s_error = L"Error";
            lvi.pszText = const_cast<LPWSTR>(s_error);
            int idx = (int)SendMessageW(g_file_list, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
            if (idx >= 0) {
                LVITEMW sub = {};
                sub.mask = LVIF_TEXT;
                sub.iItem = idx;
                sub.iSubItem = 1;
                static const wchar_t *s_failed = L"Failed to list directory";
                sub.pszText = const_cast<LPWSTR>(s_failed);
                SendMessageW(g_file_list, LVM_SETITEMTEXTW, idx, (LPARAM)&sub);
            }
            if (g_status_label) SetWindowTextW(g_status_label, L"Error");
        }
        break;
    }
    // push result
    case WM_USER + 201: {
        g_busy = false;
        if (g_on_busy) g_on_busy(false);
        {
            char buf[128];
            if ((int)lp > 0) {
                snprintf(buf, sizeof(buf), "Pushed %d, failed %d", (int)wp, (int)lp);
                show_toast(buf, false);
            } else if ((int)wp > 0) {
                snprintf(buf, sizeof(buf), "Pushed %d file(s)", (int)wp);
                show_toast(buf, true);
            } else {
                show_toast("Push failed", false);
            }
        }
        refresh_list();
        break;
    }
    // pull result
    case WM_USER + 202: {
        g_busy = false;
        if (g_on_busy) g_on_busy(false);
        {
            char buf[128];
            if ((int)wp > 0 && (int)lp == 0) {
                snprintf(buf, sizeof(buf), "Pulled %d file(s)", (int)wp);
                show_toast(buf, true);
            } else if ((int)wp > 0 && (int)lp > 0) {
                snprintf(buf, sizeof(buf), "Pulled %d, failed %d", (int)wp, (int)lp);
                show_toast(buf, false);
            } else {
                show_toast("Pull failed", false);
            }
        }
        update_status();
        break;
    }
    // delete result
    case WM_USER + 203: {
        g_busy = false;
        if (g_on_busy) g_on_busy(false);
        {
            char buf[128];
            if ((int)lp > 0) {
                snprintf(buf, sizeof(buf), "Deleted %d, failed %d", (int)wp, (int)lp);
                show_toast(buf, false);
            } else if ((int)wp > 0) {
                snprintf(buf, sizeof(buf), "Deleted %d item(s)", (int)wp);
                show_toast(buf, true);
            } else {
                show_toast("Delete failed", false);
            }
        }
        refresh_list();
        break;
    }
    // paste result
    case WM_USER + 204: {
        g_busy = false;
        if (g_on_busy) g_on_busy(false);
        {
            char buf[128];
            if ((int)lp > 0) {
                snprintf(buf, sizeof(buf), "Pasted %d, failed %d", (int)wp, (int)lp);
                show_toast(buf, false);
            } else if ((int)wp > 0) {
                snprintf(buf, sizeof(buf), "Pasted %d item(s)", (int)wp);
                show_toast(buf, true);
            } else {
                show_toast("Paste failed", false);
            }
        }
        refresh_list();
        break;
    }

    case WM_DESTROY:
        g_hwnd = nullptr;
        g_file_list = nullptr;
        g_address_edit = nullptr;
        g_folder_icon = nullptr;
        g_status_label = nullptr;
        g_entries.clear();
        g_clipboard.entries.clear();
        g_clipboard.action = FileClipAction::None;
        g_busy = false;
        if (g_img_list) { ImageList_Destroy(g_img_list); g_img_list = nullptr; }
        if (g_icon_folder) { DestroyIcon(g_icon_folder); g_icon_folder = nullptr; }
        if (g_icon_file) { DestroyIcon(g_icon_file); g_icon_file = nullptr; }
        if (g_icon_link) { DestroyIcon(g_icon_link); g_icon_link = nullptr; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace hscrcpy
