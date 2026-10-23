#include "file_manager_window.h"
#include "msgbox_util.h"
#include "log.h"
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <sstream>
#include <algorithm>
#include <set>
#include <fstream>

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
        SetPropW(hwnd, L"id_font", (HANDLE)hf);
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
    case WM_DESTROY: {
        HFONT hf = (HFONT)GetPropW(hwnd, L"id_font");
        if (hf) { DeleteObject(hf); RemovePropW(hwnd, L"id_font"); }
        break;
    }
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

    HWND owner = g_parent;
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
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); break; }
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
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
    // First try UTF-8 conversion (handles embedded NUL by using length)
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    if (len > 0) {
        std::wstring ws(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &ws[0], len);
        return ws;
    }
    // Fallback: byte-by-byte mapping (for binary/non-UTF-8 content)
    std::wstring ws;
    ws.reserve(s.size());
    for (unsigned char c : s)
        ws += (wchar_t)c;
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

// Replace a button's icon, destroying the previous one to avoid GDI leaks
static void set_btn_icon(HWND btn, HICON new_ico) {
    HICON prev = (HICON)SendMessageW(btn, BM_SETIMAGE, IMAGE_ICON, (LPARAM)new_ico);
    if (prev) DestroyIcon(prev);
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
HWND FileManagerWindow::g_selectall_btn = nullptr;
HWND FileManagerWindow::g_file_list = nullptr;
HWND FileManagerWindow::g_status_label = nullptr;
HINSTANCE FileManagerWindow::g_inst = nullptr;
HWND FileManagerWindow::g_parent = nullptr;
int FileManagerWindow::g_panel_w = 420;

std::string FileManagerWindow::g_current_dir;
std::vector<FileEntry> FileManagerWindow::g_entries;
std::vector<std::string> FileManagerWindow::g_history;
FileClipboard FileManagerWindow::g_clipboard;
std::string FileManagerWindow::g_select_after_refresh;
bool FileManagerWindow::g_check_mode = false;

FileManagerWindow::OnShellCommand FileManagerWindow::g_on_shell;
FileManagerWindow::OnPushFile FileManagerWindow::g_on_push;
FileManagerWindow::OnPullFile FileManagerWindow::g_on_pull;
FileManagerWindow::OnBusy FileManagerWindow::g_on_busy;
bool FileManagerWindow::g_busy = false;

HIMAGELIST FileManagerWindow::g_img_list = nullptr;
HWND FileManagerWindow::g_toast_hwnd = nullptr;

// Error detail strings shared between worker threads and WndProc
static std::string s_last_error;   // push/import
static std::string s_pull_error;   // pull/export
static std::string s_del_error;    // delete
static std::string s_paste_error;  // paste

// CBT hook to center MessageBox on the file manager panel
// ── Custom message/confirm dialogs (replaces MessageBoxW) ──
// MessageBoxW with WS_CHILD owner causes the main window to disappear.
// Custom popup dialogs with WS_EX_TOPMOST + own modal loop avoid this.

static HWND s_fm_hwnd_for_dialog = nullptr;  // set by FileManagerWindow::show()

struct CustomMsgData {
    std::wstring text;
    std::wstring title;
    bool is_confirm;  // true = Yes/No, false = OK only
    bool yes_clicked;
};

static LRESULT CALLBACK CustomMsgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *data = reinterpret_cast<CustomMsgData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto *cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        data = reinterpret_cast<CustomMsgData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        SetWindowTextW(hwnd, data->title.c_str());

        // Measure text to size dialog
        HDC hdc = GetDC(hwnd);
        HFONT hf = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(hdc, hf);
        RECT tr = {0, 0, 400, 0};
        DrawTextW(hdc, data->text.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        ReleaseDC(hwnd, hdc);

        int text_w = tr.right - tr.left + 24;
        int text_h = tr.bottom - tr.top;
        int dlg_w = text_w + 24;
        if (dlg_w < 280) dlg_w = 280;
        if (dlg_w > 500) dlg_w = 500;

        // Account for non-client area (title bar + borders ~30px) and button row (48px)
        const int btn_row_h = 48;
        const int nc_h = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFIXEDFRAME) * 2;
        int client_h = text_h + 12 + btn_row_h;  // 12px top margin before text
        int max_client_h = GetSystemMetrics(SM_CYSCREEN) - nc_h - 80;  // leave margin
        if (client_h > max_client_h) client_h = max_client_h;
        if (client_h < 100) client_h = 100;

        // Adjust window rect: client_h -> total dlg_h
        RECT adj = {0, 0, dlg_w, client_h};
        AdjustWindowRectEx(&adj, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME | WS_EX_TOPMOST);
        int dlg_h = adj.bottom - adj.top;

        // Visible text area height (may be less than full text_h if clipped)
        int vis_text_h = client_h - 12 - btn_row_h;
        if (vis_text_h < 40) vis_text_h = 40;

        // Resize and center dialog
        RECT wr;
        GetWindowRect(hwnd, &wr);
        int cx = wr.left + (wr.right - wr.left - dlg_w) / 2;
        int cy = wr.top + (wr.bottom - wr.top - dlg_h) / 2;
        MoveWindow(hwnd, cx, cy, dlg_w, dlg_h, FALSE);

        // Text area — use read-only EDIT with scrollbar for long content
        // No ES_AUTOHSCROLL so text wraps; convert \n to \r\n for Windows EDIT
        DWORD edit_style = WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_LEFT;
        if (text_h > vis_text_h)
            edit_style |= WS_VSCROLL;
        std::wstring fixed_text = data->text;
        // Replace standalone \n (not preceded by \r) with \r\n
        std::wstring result;
        result.reserve(fixed_text.size());
        for (size_t i = 0; i < fixed_text.size(); i++) {
            if (fixed_text[i] == L'\n' && (i == 0 || fixed_text[i - 1] != L'\r'))
                result += L"\r\n";
            else
                result += fixed_text[i];
        }
        CreateWindowExW(0, L"EDIT", result.c_str(),
            edit_style,
            12, 12, dlg_w - 24, vis_text_h, hwnd, NULL, cs->hInstance, NULL);

        int btn_y = 12 + vis_text_h + 8;
        if (data->is_confirm) {
            CreateWindowW(L"BUTTON", L"Yes",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                dlg_w / 2 - 90, btn_y, 80, 28, hwnd, (HMENU)IDYES, cs->hInstance, NULL);
            CreateWindowW(L"BUTTON", L"No",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                dlg_w / 2 + 10, btn_y, 80, 28, hwnd, (HMENU)IDNO, cs->hInstance, NULL);
        } else {
            CreateWindowW(L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                dlg_w / 2 - 40, btn_y, 80, 28, hwnd, (HMENU)IDOK, cs->hInstance, NULL);
        }

        SendMessageW(hwnd, WM_SETFONT, (WPARAM)hf, TRUE);
        EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE;
        }, (LPARAM)hf);
        SetPropW(hwnd, L"cm_font", (HANDLE)hf);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDYES) {
            if (data) data->yes_clicked = true;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (LOWORD(wp) == IDNO) {
            if (data) data->yes_clicked = false;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        HFONT hf = (HFONT)GetPropW(hwnd, L"cm_font");
        if (hf) { DeleteObject(hf); RemovePropW(hwnd, L"cm_font"); }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Show a custom popup dialog. Returns true for Yes (confirm) or always for OK (msg_box).
static bool show_custom_dialog(const std::wstring &title, const std::wstring &text, bool is_confirm) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CustomMsgProc;
        wc.hInstance = (HINSTANCE)GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HScrcpyCustomMsgClass";
        RegisterClassExW(&wc);
        reg = true;
    }

    CustomMsgData data;
    data.text = text;
    data.title = title;
    data.is_confirm = is_confirm;
    data.yes_clicked = false;

    // Center on main window
    RECT orr = {};
    HWND main_wnd = s_fm_hwnd_for_dialog ? GetParent(s_fm_hwnd_for_dialog) : nullptr;
    if (main_wnd)
        GetWindowRect(main_wnd, &orr);
    else if (s_fm_hwnd_for_dialog)
        GetWindowRect(s_fm_hwnd_for_dialog, &orr);
    else if (GetForegroundWindow())
        GetWindowRect(GetForegroundWindow(), &orr);
    int cx = orr.left + (orr.right - orr.left - 360) / 2;
    int cy = orr.top + (orr.bottom - orr.top - 160) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"HScrcpyCustomMsgClass", title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        cx, cy, 360, 160,
        NULL, NULL, (HINSTANCE)GetModuleHandle(NULL), &data);

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    // Modal message loop
    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, NULL, 0, 0)) {
        if (!IsWindow(dlg)) break;
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); break; }
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    return data.yes_clicked;
}

// ── Text viewer dialog ──────────────────────────────────

// Forward declarations for functions defined later in this file
static std::string shell_escape(const std::string &s);
static std::string wide_to_utf8(const std::wstring &ws);

// SHBrowseForFolder callback: center on g_parent + optional initial dir.
// pData = char* path for BFFM_SETSELECTIONA (pass 0 to skip).
// Shared by download / import_files / import_folder / export_files.
int CALLBACK FileManagerWindow::browse_center_callback(HWND hwnd, UINT uMsg, LPARAM, LPARAM pData) {
    if (uMsg != BFFM_INITIALIZED) return 0;
    if (pData) SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, pData);
    if (g_parent) {
        RECT orr, mr;
        if (GetWindowRect(g_parent, &orr) && GetWindowRect(hwnd, &mr)) {
            int cx = orr.left + ((orr.right - orr.left) - (mr.right - mr.left)) / 2;
            int cy = orr.top + ((orr.bottom - orr.top) - (mr.bottom - mr.top)) / 2;
            SetWindowPos(hwnd, NULL, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
    }
    return 0;
}

struct TextViewerData {
    std::wstring title;
    std::wstring content;     // already \r\n converted
    std::string remote_path;  // full device path for save-back (empty = read-only)
    bool editable;            // true = show Save button, EDIT is writable
};

// Text viewer control IDs
enum TvCtrlId : int {
    TV_EDIT = 4001,
    TV_SAVE_BTN = 4002,
};

// EDIT control subclass procedure to handle Ctrl+A
static WNDPROC g_orig_edit_proc = nullptr;
static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        // Ctrl+A: select all text
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    return CallWindowProcW(g_orig_edit_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK TextViewerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *data = reinterpret_cast<TextViewerData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto *cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        data = reinterpret_cast<TextViewerData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        SetWindowTextW(hwnd, data->title.c_str());

        HFONT hf = CreateFontW(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");

        RECT rc;
        GetClientRect(hwnd, &rc);

        // EDIT control — editable or read-only based on data->editable
        DWORD edit_style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                           ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_LEFT;
        if (!data->editable) edit_style |= ES_READONLY;
        HWND edit = CreateWindowExW(0, L"EDIT", L"", edit_style,
            0, 0, rc.right, rc.bottom - 36, hwnd, (HMENU)TV_EDIT, cs->hInstance, NULL);
        // Subclass EDIT to handle Ctrl+A
        g_orig_edit_proc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
        SendMessageW(edit, WM_SETFONT, (WPARAM)hf, TRUE);
        // Allow large text (default EDIT limit is ~32K)
        SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);
        SetWindowTextW(edit, data->content.c_str());

        if (data->editable) {
            // [Save] [Close] buttons
            int bw = 80, gap = 8;
            int total = bw * 2 + gap;
            int x1 = rc.right / 2 - total / 2;
            CreateWindowW(L"BUTTON", L"Save",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                x1, rc.bottom - 32, bw, 26, hwnd, (HMENU)TV_SAVE_BTN, cs->hInstance, NULL);
            CreateWindowW(L"BUTTON", L"Close",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                x1 + bw + gap, rc.bottom - 32, bw, 26, hwnd, (HMENU)IDOK, cs->hInstance, NULL);
            SendMessageW(GetDlgItem(hwnd, TV_SAVE_BTN), WM_SETFONT, (WPARAM)hf, TRUE);
            SendMessageW(GetDlgItem(hwnd, IDOK), WM_SETFONT, (WPARAM)hf, TRUE);
        } else {
            // Read-only: just [Close] button
            CreateWindowW(L"BUTTON", L"Close",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                rc.right / 2 - 40, rc.bottom - 32, 80, 26, hwnd, (HMENU)IDOK, cs->hInstance, NULL);
            SendMessageW(GetDlgItem(hwnd, IDOK), WM_SETFONT, (WPARAM)hf, TRUE);
        }

        // Store font and dirty flag as window props
        SetPropW(hwnd, L"tv_font", (HANDLE)hf);
        SetPropW(hwnd, L"tv_dirty", (HANDLE)FALSE);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HWND edit = GetDlgItem(hwnd, TV_EDIT);
        if (edit) MoveWindow(edit, 0, 0, rc.right, rc.bottom - 36, TRUE);
        if (data && data->editable) {
            int bw = 80, gap = 8;
            int total = bw * 2 + gap;
            int x1 = rc.right / 2 - total / 2;
            HWND save_btn = GetDlgItem(hwnd, TV_SAVE_BTN);
            HWND close_btn = GetDlgItem(hwnd, IDOK);
            if (save_btn) MoveWindow(save_btn, x1, rc.bottom - 32, bw, 26, TRUE);
            if (close_btn) MoveWindow(close_btn, x1 + bw + gap, rc.bottom - 32, bw, 26, TRUE);
        } else {
            HWND btn = GetDlgItem(hwnd, IDOK);
            if (btn) MoveWindow(btn, rc.right / 2 - 40, rc.bottom - 32, 80, 26, TRUE);
        }
        // Force repaint to prevent ghosting artifacts
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == TV_EDIT && code == EN_CHANGE) {
            // Mark dirty on any edit
            SetPropW(hwnd, L"tv_dirty", (HANDLE)TRUE);
            return 0;
        }
        if (id == TV_SAVE_BTN) {
            // Save edited content back to device
            if (!data || data->remote_path.empty()) return 0;

            HWND edit = GetDlgItem(hwnd, TV_EDIT);
            int len = GetWindowTextLengthW(edit);
            std::wstring wtext(len, L'\0');
            GetWindowTextW(edit, &wtext[0], len + 1);

            // Convert \r\n → \n, wide → UTF-8
            std::wstring cleaned;
            cleaned.reserve(wtext.size());
            for (size_t i = 0; i < wtext.size(); i++) {
                if (wtext[i] == L'\r' && i + 1 < wtext.size() && wtext[i + 1] == L'\n')
                    continue;  // skip \r, keep \n
                cleaned += wtext[i];
            }
            std::string utf8 = wide_to_utf8(cleaned);

            // Write to local temp file
            std::string local_tmp = get_download_dir() + ".hscrcpy_edit_tmp";
            {
                std::ofstream ofs(local_tmp, std::ios::binary | std::ios::trunc);
                if (!ofs) {
                    HWND main_wnd_tv = s_fm_hwnd_for_dialog ? GetParent(s_fm_hwnd_for_dialog) : nullptr;
                    hscrcpy::centered_msgbox_w(hwnd, L"Cannot write temp file.", L"Save Failed", MB_OK | MB_ICONERROR, main_wnd_tv);
                    return 0;
                }
                ofs.write(utf8.data(), utf8.size());
            }

            // Post save request to file manager WndProc (which has access to g_on_shell etc.)
            auto *save_data = new std::pair<std::string, std::string>(
                std::move(local_tmp), data->remote_path);
            PostMessageW(s_fm_hwnd_for_dialog, WM_USER + 211, (WPARAM)hwnd, (LPARAM)save_data);

            // Disable Save button while saving
            EnableWindow(GetDlgItem(hwnd, TV_SAVE_BTN), FALSE);
            return 0;
        }
        if (id == IDOK)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_CLOSE: {
        // Check dirty state — ask to discard if unsaved
        BOOL dirty = (BOOL)(LONG_PTR)GetPropW(hwnd, L"tv_dirty");
        if (dirty) {
            int r = hscrcpy::centered_msgbox_w(hwnd, L"Discard unsaved changes?", L"Confirm",
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2, s_fm_hwnd_for_dialog ? GetParent(s_fm_hwnd_for_dialog) : nullptr);
            if (r != IDYES) return 0;  // stay open
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY: {
        // Restore EDIT control's original window procedure
        HWND edit = GetDlgItem(hwnd, TV_EDIT);
        if (edit && g_orig_edit_proc) {
            SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)g_orig_edit_proc);
            g_orig_edit_proc = nullptr;
        }
        HFONT hf = (HFONT)GetPropW(hwnd, L"tv_font");
        if (hf) { DeleteObject(hf); RemovePropW(hwnd, L"tv_font"); }
        RemovePropW(hwnd, L"tv_dirty");
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_text_viewer(const std::wstring &title, const std::wstring &content,
                              const std::string &remote_path = "", bool editable = true) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TextViewerProc;
        wc.hInstance = (HINSTANCE)GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HScrcpyTextViewerClass";
        RegisterClassExW(&wc);
        reg = true;
    }

    TextViewerData data;
    data.title = title;
    data.content = content;
    data.remote_path = remote_path;
    data.editable = editable && !remote_path.empty();

    // Center on main window
    RECT orr = {};
    HWND main_wnd = s_fm_hwnd_for_dialog ? GetParent(s_fm_hwnd_for_dialog) : nullptr;
    if (main_wnd)
        GetWindowRect(main_wnd, &orr);
    else if (s_fm_hwnd_for_dialog)
        GetWindowRect(s_fm_hwnd_for_dialog, &orr);
    else if (GetForegroundWindow())
        GetWindowRect(GetForegroundWindow(), &orr);

    int dlg_w = 560, dlg_h = 440;
    int cx = orr.left + (orr.right - orr.left - dlg_w) / 2;
    int cy = orr.top + (orr.bottom - orr.top - dlg_h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"HScrcpyTextViewerClass", title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
        cx, cy, dlg_w, dlg_h,
        NULL, NULL, (HINSTANCE)GetModuleHandle(NULL), &data);

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    // Modal message loop — don't use IsDialogMessageW so that
    // Enter key goes to the EDIT control instead of being swallowed
    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, NULL, 0, 0)) {
        if (!IsWindow(dlg)) break;
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); break; }
        // Intercept Esc key to close the text viewer dialog
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && IsWindow(dlg)) {
            PostMessageW(dlg, WM_CLOSE, 0, 0);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

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
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
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
    s_fm_hwnd_for_dialog = g_hwnd;

    ShowWindow(g_hwnd, SW_SHOW);
    if (g_current_dir.empty()) navigate("/");
    else refresh_list();
}

void FileManagerWindow::hide() {
    if (g_check_mode) exit_select_mode();
    if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
}

void FileManagerWindow::reset() {
    if (g_check_mode) {
        g_check_mode = false;
        if (g_file_list) SendMessageW(g_file_list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES, 0);
    }
    if (g_file_list) ListView_DeleteAllItems(g_file_list);
    if (g_address_edit) SetWindowTextW(g_address_edit, L"");
    if (g_search_edit) SetWindowTextW(g_search_edit, L"");
    if (g_status_label) SetWindowTextW(g_status_label, L"");
    g_current_dir.clear();
    g_entries.clear();
    g_history.clear();
    g_clipboard = FileClipboard{};
    g_select_after_refresh.clear();
    g_busy = false;
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

// ── File type classification ─────────────────────────────

enum FileCategory { kText, kImage, kBinary, kOther };

static FileCategory get_file_category(const std::string &name) {
    // Handle .z.so double extension first
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".z.so") == 0) return kBinary;

    auto dot_pos = name.rfind('.');
    if (dot_pos == std::string::npos) return kOther;  // no extension → sniff content later
    std::string ext = name.substr(dot_pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    static const std::set<std::string> text_exts = {
        "txt", "log", "cfg", "conf", "ini", "json", "xml", "yaml", "yml", "toml",
        "md", "csv", "html", "htm", "css", "js", "ts", "py", "sh", "bat", "cmd",
        "c", "cpp", "h", "hpp", "java", "kt", "rs", "go", "rb", "php", "sql",
        "properties", "gradle", "cmake", "makefile", "gitignore", "dockerfile",
        "rc", "reg", "inf", "prop", "version", "cert", "pem", "csr",
        "map", "plist", "pro", "mk", "am"
    };
    static const std::set<std::string> image_exts = {
        "jpg", "jpeg", "png", "gif", "bmp", "webp", "ico", "svg", "tiff", "tif"
    };
    static const std::set<std::string> binary_exts = {
        "so", "o", "a", "bin", "dex", "jar", "apk", "hap", "db", "sqlite",
        "proto", "pb", "elf", "ko", "dat", "pak"
    };

    if (text_exts.count(ext)) return kText;
    if (image_exts.count(ext)) return kImage;
    if (binary_exts.count(ext)) return kBinary;
    return kOther;
}

// Sniff file content to determine if it's text or binary.
// Reads first 512 bytes and checks for null bytes (binary indicator).
static FileCategory sniff_content_category(const std::string &content) {
    size_t check_len = (std::min)(content.size(), (size_t)512);
    if (check_len == 0) return kText;  // empty file → treat as text
    for (size_t i = 0; i < check_len; i++) {
        unsigned char c = static_cast<unsigned char>(content[i]);
        // Null byte = definitely binary
        if (c == 0) return kBinary;
        // Non-printable, non-whitespace control chars (except \t \n \r) = likely binary
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return kBinary;
    }
    return kText;
}

// Get user's Downloads directory for pulled files
std::string get_download_dir() {
    PWSTR downloads_path = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &downloads_path);
    if (SUCCEEDED(hr) && downloads_path) {
        std::string result;
        int len = WideCharToMultiByte(CP_ACP, 0, downloads_path, -1, NULL, 0, NULL, NULL);
        if (len > 0) {
            result.resize(len - 1);
            WideCharToMultiByte(CP_ACP, 0, downloads_path, -1, &result[0], len, NULL, NULL);
        }
        CoTaskMemFree(downloads_path);
        if (!result.empty()) return result + "\\";
    }
    // Fallback to Desktop
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path))) {
        return std::string(path) + "\\";
    }
    return ".\\";
}


// Struct for WM_USER+210 (pull-and-open result)
struct PullOpenResult {
    std::string local_path;
    std::string file_name;
    FileCategory category;
    bool ok;
};

// Struct for WM_USER+205 (text file load result)
struct TextLoadResult {
    std::string file_name;    // display name
    std::string remote_path;  // full device path for save-back
    std::wstring content;     // \r\n-converted content
    bool read_ok;             // false if cat failed → read-only viewer
};



void FileManagerWindow::enter_selected() {
    int sel = ListView_GetNextItem(g_file_list, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_entries.size()) return;

    const FileEntry &e = g_entries[sel];
    if (e.is_dir()) {
        // Directory — navigate into it
        if (!e.name.empty() && e.name[0] == '/') {
            navigate(e.name);
        } else {
            navigate(build_remote_path(g_current_dir, e.name));
        }
    } else if (e.is_link()) {
        // Symlink — navigate to the link target
        std::string target = e.link_target.empty()
            ? build_remote_path(g_current_dir, e.name)
            : e.link_target;
        // Resolve relative target to absolute path
        if (!target.empty() && target[0] != '/') {
            target = build_remote_path(g_current_dir, target);
        }
        // Check if target is a directory or file
        std::string check_output;
        bool is_dir = false;
        if (g_on_shell) {
            g_on_shell("ls -ald " + shell_escape(target), &check_output, 5);
            is_dir = !check_output.empty() && check_output[0] == 'd';
        }
        if (is_dir) {
            navigate(target);
        } else {
            // Target is a file — navigate to its parent directory and select it
            auto last_sep = target.rfind('/');
            if (last_sep != std::string::npos && last_sep > 0) {
                std::string parent_dir = target.substr(0, last_sep + 1);
                std::string file_name = target.substr(last_sep + 1);
                g_select_after_refresh = file_name;
                navigate(parent_dir);
            } else {
                navigate(target);
            }
        }
    } else if (e.is_file()) {
        // Double-click on a file
        if (!e.can_read()) {
            msg_box("Permission Denied", "Cannot read '%s': %s", e.name.c_str(), e.perms.c_str());
            return;
        }

        // Build remote path
        std::string path;
        if (!e.name.empty() && e.name[0] == '/') {
            path = e.name;
        } else {
            path = ensure_dir_sep(g_current_dir) + e.name;
        }

        const long long MAX_VIEW_SIZE = 1024 * 1024;  // 1 MB
        FileCategory cat = get_file_category(e.name);

        // For unknown extensions (kOther), sniff content to determine text vs binary
        // We'll handle this inside the background thread after reading the first chunk

        // Helper: pull file to user-chosen folder (binary / large-unknown)
        auto file_name_copy = e.name;
        auto file_path_copy = path;
        auto pull_binary_save = [file_name_copy, file_path_copy]() {
            char save_dir[MAX_PATH] = {};
            std::string default_dir = get_download_dir();
            if (!default_dir.empty() && default_dir.back() == '\\') default_dir.pop_back();
            BROWSEINFOA bi = {};
            bi.hwndOwner = g_parent;
            bi.lpszTitle = "Select folder to save file";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            bi.lParam = (LPARAM)default_dir.c_str();
            bi.lpfn = browse_center_callback;
            LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
            if (!pidl) return;
            SHGetPathFromIDListA(pidl, save_dir);
            CoTaskMemFree(pidl);

            g_busy = true;
            if (g_on_busy) g_on_busy(true);
            if (g_status_label) SetWindowTextW(g_status_label, L"Downloading file...");

            DWORD tid;
            CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
                auto *d = reinterpret_cast<std::tuple<std::string, std::string, std::string>*>(param);
                auto &name = std::get<0>(*d);
                auto &remote = std::get<1>(*d);
                auto &local_dir = std::get<2>(*d);

                auto *result = new PullOpenResult();
                result->file_name = name;
                result->category = kBinary;
                result->local_path = local_dir + "\\" + name;
                std::string pull_output;
                result->ok = g_on_pull(remote, result->local_path, &pull_output);

                PostMessageW(g_hwnd, WM_USER + 210, 0, (LPARAM)result);
                delete d;
                return 0;
            }, new std::tuple<std::string, std::string, std::string>(
                std::move(file_name_copy), std::move(file_path_copy), std::string(save_dir)
            ), 0, &tid);
        };

        switch (cat) {
        case kText: {
            if (e.size > MAX_VIEW_SIZE) {
                export_files();
                return;
            }
            // Cat file content and show in text viewer
            g_busy = true;
            if (g_on_busy) g_on_busy(true);
            if (g_status_label) SetWindowTextW(g_status_label, L"Loading file...");

            auto file_name = e.name;
            auto file_path = path;
            DWORD tid;
            CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
                auto *d = reinterpret_cast<std::pair<std::string, std::string>*>(param);
                auto &name = d->first;
                auto &path = d->second;

                auto *result = new TextLoadResult();
                result->file_name = name;
                result->remote_path = path;

                std::string output;
                bool ok = g_on_shell("cat " + shell_escape(path), &output, 15);
                // If cat returned empty (HDC truncates at null bytes), try cat -v
                // cat -v displays non-printable chars as ^@/M-X but breaks UTF-8
                // multi-byte chars (Chinese etc.), so only use as fallback
                if (ok && output.empty()) {
                    g_on_shell("cat -v " + shell_escape(path), &output, 15);
                }
                if (ok) {
                    // Convert \n to \r\n for Windows EDIT control
                    std::wstring wide = utf8_to_wide(output);
                    result->content.reserve(wide.size());
                    for (size_t i = 0; i < wide.size(); i++) {
                        if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r'))
                            result->content += L"\r\n";
                        else
                            result->content += wide[i];
                    }
                    result->read_ok = true;
                } else {
                    result->content = L"Failed to read file: " + utf8_to_wide(name);
                    if (!output.empty()) result->content += L"\r\n" + utf8_to_wide(output);
                    result->read_ok = false;
                }

                PostMessageW(g_hwnd, WM_USER + 205, 0, (LPARAM)result);

                delete d;
                return 0;
            }, new std::pair<std::string, std::string>(std::move(file_name), std::move(file_path)), 0, &tid);
            break;
        }
        case kImage: {
            // Pull to Downloads dir and open with Windows viewer
            g_busy = true;
            if (g_on_busy) g_on_busy(true);
            if (g_status_label) SetWindowTextW(g_status_label, L"Downloading image...");

            auto file_name = e.name;
            auto file_path = path;
            DWORD tid;
            CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
                auto *d = reinterpret_cast<std::pair<std::string, std::string>*>(param);
                auto &name = d->first;
                auto &remote = d->second;
                auto *result = new PullOpenResult();
                result->file_name = name;
                result->category = kImage;

                std::string local_dir = get_download_dir();
                result->local_path = local_dir + name;
                std::string pull_output;
                result->ok = g_on_pull(remote, result->local_path, &pull_output);

                PostMessageW(g_hwnd, WM_USER + 210, 0, (LPARAM)result);
                delete d;
                return 0;
            }, new std::pair<std::string, std::string>(std::move(file_name), std::move(file_path)), 0, &tid);
            break;
        }
        case kBinary:
            // Binary file — ask user where to save
            pull_binary_save();
            break;
        case kOther:
            if (e.size > MAX_VIEW_SIZE) {
                // Large unknown file — treat as binary
                pull_binary_save();
            } else {
                // Small unknown file — sniff content to determine text vs binary
                g_busy = true;
                if (g_on_busy) g_on_busy(true);
                if (g_status_label) SetWindowTextW(g_status_label, L"Loading file...");

                auto file_name = e.name;
                auto file_path = path;
                DWORD tid;
                CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
                    auto *d = reinterpret_cast<std::pair<std::string, std::string>*>(param);
                    auto &name = d->first;
                    auto &path = d->second;

                    // Read content and sniff
                    std::string output;
                    bool ok = g_on_shell("cat " + shell_escape(path), &output, 15);
                    if (ok) {
                        FileCategory sniffed = sniff_content_category(output);
                        if (sniffed == kText) {
                            // It's text — use raw cat output for display
                            // If cat returned empty (HDC truncates at null bytes),
                            // fall back to cat -v (breaks UTF-8 multi-byte but shows something)
                            std::string &display_output = output;
                            std::string catv_output;
                            if (output.empty()) {
                                g_on_shell("cat -v " + shell_escape(path), &catv_output, 15);
                                if (!catv_output.empty()) display_output = catv_output;
                            }
                            auto *result = new TextLoadResult();
                            result->file_name = name;
                            result->remote_path = path;
                            result->read_ok = true;
                            std::wstring wide = utf8_to_wide(display_output);
                            result->content.reserve(wide.size());
                            for (size_t i = 0; i < wide.size(); i++) {
                                if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r'))
                                    result->content += L"\r\n";
                                else
                                    result->content += wide[i];
                            }
                            PostMessageW(g_hwnd, WM_USER + 205, 0, (LPARAM)result);
                        } else {
                            // It's binary — pull to Downloads and notify
                            auto *result = new PullOpenResult();
                            result->file_name = name;
                            result->category = kBinary;
                            result->local_path = get_download_dir() + name;
                            std::string pull_output;
                            result->ok = g_on_pull(path, result->local_path, &pull_output);
                            PostMessageW(g_hwnd, WM_USER + 210, 0, (LPARAM)result);
                        }
                    } else {
                        // Failed to read — show error in text viewer
                        auto *result = new TextLoadResult();
                        result->file_name = name;
                        result->remote_path = path;
                        result->read_ok = false;
                        result->content = L"Failed to read file: " + utf8_to_wide(name);
                        if (!output.empty()) result->content += L"\r\n" + utf8_to_wide(output);
                        PostMessageW(g_hwnd, WM_USER + 205, 0, (LPARAM)result);
                    }

                    delete d;
                    return 0;
                }, new std::pair<std::string, std::string>(std::move(file_name), std::move(file_path)), 0, &tid);
            }
            break;
        }
    }
}

// ── Refresh ──────────────────────────────────────────────

void FileManagerWindow::refresh_list() {
    if (!g_on_shell || g_busy || !g_hwnd) return;
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
    if (g_hwnd) PostMessageW(g_hwnd, WM_USER + 200, result, 0);
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
        // Escape single quotes in directory path for shell safety
        std::string escaped_dir;
        for (char c : norm_dir) {
            if (c == '\'') escaped_dir += "'\\''";
            else escaped_dir += c;
        }
        script += "DIR='" + escaped_dir + "'\n";
        // Escape single quotes in search query for shell safety
        std::string escaped_query;
        for (char c : query) {
            if (c == '\'') escaped_query += "'\\''";
            else escaped_query += c;
        }
        script += "Q='*" + escaped_query + "*'\n";
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
                g_on_push(local_script, remote_script, nullptr);
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

    // Ask: files or folder?
    // 0 = cancelled, 1 = files, 2 = folder
    int choice = 0;
    {
        struct ImportChoiceData { int *result; };
        ImportChoiceData icd = { &choice };

        static bool reg_imp = false;
        if (!reg_imp) {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                auto *icd = reinterpret_cast<ImportChoiceData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                switch (msg) {
                case WM_CREATE: {
                    auto *cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    icd = reinterpret_cast<ImportChoiceData*>(cs->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)icd);

                    CreateWindowW(L"STATIC", L"Import:",
                        WS_CHILD | WS_VISIBLE, 12, 14, 200, 18, hwnd, NULL, cs->hInstance, NULL);
                    CreateWindowW(L"BUTTON", L"Folder",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                        12, 44, 90, 28, hwnd, (HMENU)1001, cs->hInstance, NULL);
                    CreateWindowW(L"BUTTON", L"Files",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        112, 44, 90, 28, hwnd, (HMENU)1002, cs->hInstance, NULL);
                    CreateWindowW(L"BUTTON", L"Cancel",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        212, 44, 90, 28, hwnd, (HMENU)1003, cs->hInstance, NULL);

                    HFONT hf = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                    SendMessageW(hwnd, WM_SETFONT, (WPARAM)hf, TRUE);
                    EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
                        SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE;
                    }, (LPARAM)hf);
                    SetPropW(hwnd, L"ic_font", (HANDLE)hf);
                    return 0;
                }
                case WM_COMMAND:
                    if (LOWORD(wp) == 1001) { *icd->result = 2; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
                    else if (LOWORD(wp) == 1002) { *icd->result = 1; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
                    else if (LOWORD(wp) == 1003) { *icd->result = 0; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
                    return 0;
                case WM_CLOSE:
                    DestroyWindow(hwnd);
                    return 0;
                case WM_DESTROY: {
                    HFONT hf = (HFONT)GetPropW(hwnd, L"ic_font");
                    if (hf) { DeleteObject(hf); RemovePropW(hwnd, L"ic_font"); }
                    break;
                }
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            };
            wc.hInstance = g_inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = L"HScrcpyImportChoiceDlg";
            RegisterClassExW(&wc);
            reg_imp = true;
        }

        RECT orr;
        GetWindowRect(g_hwnd, &orr);
        HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
            L"HScrcpyImportChoiceDlg", L"Import to Device",
            WS_POPUP | WS_CAPTION,
            orr.left + (orr.right - orr.left - 320) / 2,
            orr.top + (orr.bottom - orr.top - 120) / 2,
            320, 120, g_parent, NULL, g_inst, &icd);

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

    if (choice == 0) return;

    std::vector<std::string> files;

    if (choice == 1) {
        // Select files via IFileOpenDialog
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        IFileOpenDialog *pfd = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&pfd));
        if (SUCCEEDED(hr) && pfd) {
            DWORD flags;
            pfd->GetOptions(&flags);
            pfd->SetOptions(flags | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST);
            pfd->SetTitle(L"Select files to push to device");

            if (SUCCEEDED(pfd->Show(g_parent))) {
                IShellItemArray *pItems = nullptr;
                if (SUCCEEDED(pfd->GetResults(&pItems)) && pItems) {
                    DWORD count = 0;
                    pItems->GetCount(&count);
                    for (DWORD i = 0; i < count; i++) {
                        IShellItem *pItem = nullptr;
                        if (SUCCEEDED(pItems->GetItemAt(i, &pItem)) && pItem) {
                            LPWSTR pPath = nullptr;
                            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath)) && pPath) {
                                char mb[MAX_PATH] = {};
                                WideCharToMultiByte(CP_ACP, 0, pPath, -1, mb, MAX_PATH, NULL, NULL);
                                files.push_back(mb);
                                CoTaskMemFree(pPath);
                            }
                            pItem->Release();
                        }
                    }
                    pItems->Release();
                }
            }
            pfd->Release();
        }
        CoUninitialize();
    } else {
        // Select folder
        BROWSEINFOA bi = {};
        bi.hwndOwner = g_parent;
        bi.lpszTitle = "Select folder to push to device";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        bi.lpfn = browse_center_callback;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (!pidl) return;
        char folder_buf[MAX_PATH] = {};
        SHGetPathFromIDListA(pidl, folder_buf);
        CoTaskMemFree(pidl);
        std::string folder = folder_buf;
        if (folder.empty()) return;
        files.push_back(folder);
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
            std::string push_output;
            bool pushed = g_on_push(local, remote, &push_output);
            // hdc may return exit code 0 even on failure, check output too
            if (pushed && !shell_output_has_error(push_output)) {
                // Set permissions: 755 for directories, 644 for files
                DWORD attrs = GetFileAttributesA(local.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    g_on_shell("chmod -R 755 " + shell_escape(remote), nullptr, 10);
                } else {
                    g_on_shell("chmod 644 " + shell_escape(remote), nullptr, 5);
                }
                ok++;
            } else {
                fail++;
                if (!errors.empty()) errors += "\n";
                std::string reason = push_output.empty() ? "transfer failed" : push_output.substr(0, 120);
                errors += "Failed: " + name + " (" + reason + ")";
            }
        }

        delete data;

        s_last_error = errors;

        PostMessageW(g_hwnd, WM_USER + 201, ok, fail);
        return 0;
    }, new std::pair<std::vector<std::string>, std::string>(std::move(local_files), std::move(remote_dir)), 0, &tid);
}

void FileManagerWindow::import_folder() {
    if (!g_on_push || !g_on_shell) return;

    // Folder browser dialog
    BROWSEINFOA bi = {};
    bi.hwndOwner = g_parent;
    bi.lpszTitle = "Select folder to push to device";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_center_callback;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return;
    char folder_buf[MAX_PATH] = {};
    SHGetPathFromIDListA(pidl, folder_buf);
    CoTaskMemFree(pidl);
    std::string folder = folder_buf;
    if (folder.empty()) return;

    g_busy = true;
    if (g_on_busy) g_on_busy(true);
    if (g_status_label) SetWindowTextW(g_status_label, L"Importing folder...");

    auto local_folder = folder;
    auto remote_dir = g_current_dir;

    DWORD tid;
    CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
        auto *data = reinterpret_cast<std::pair<std::string, std::string>*>(param);
        auto &local = data->first;
        auto &remote_dir = data->second;

        std::string name = local;
        auto bs = name.find_last_of("\\/");
        if (bs != std::string::npos) name = name.substr(bs + 1);

        std::string remote = ensure_dir_sep(remote_dir) + name;
        std::string push_output;
        bool ok = g_on_push(local, remote, &push_output);
        if (ok && !shell_output_has_error(push_output)) {
            g_on_shell("chmod -R 755 " + shell_escape(remote), nullptr, 10);
        } else {
            ok = false;
        }

        delete data;

        s_last_error = ok ? "" : ("Failed: " + name + (push_output.empty() ? "" : " (" + push_output.substr(0, 120) + ")"));
        PostMessageW(g_hwnd, WM_USER + 201, ok ? 1 : 0, ok ? 0 : 1);
        return 0;
    }, new std::pair<std::string, std::string>(std::move(local_folder), std::move(remote_dir)), 0, &tid);
}

void FileManagerWindow::export_files() {
    if (!g_on_pull) return;

    // Get selected entries
    std::vector<int> selected = get_selected_indices();
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

    // Select local destination folder — default to Windows Downloads
    char folder_buf[MAX_PATH] = {};
    // Try Downloads folder via SHGetKnownFolderPath (Vista+)
    PWSTR downloads_path = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &downloads_path);
    if (SUCCEEDED(hr) && downloads_path) {
        WideCharToMultiByte(CP_ACP, 0, downloads_path, -1, folder_buf, MAX_PATH, NULL, NULL);
        CoTaskMemFree(downloads_path);
    } else {
        // Fallback: USERPROFILE\Downloads
        const char *userprofile = getenv("USERPROFILE");
        if (userprofile) {
            snprintf(folder_buf, MAX_PATH, "%s\\Downloads", userprofile);
        }
    }

    BROWSEINFOA bi = {};
    bi.hwndOwner = g_parent;
    bi.lpszTitle = "Select destination folder on PC";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lParam = (LPARAM)folder_buf;  // initial selection (Downloads)
    bi.lpfn = browse_center_callback;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return;
    char selected_buf[MAX_PATH] = {};
    SHGetPathFromIDListA(pidl, selected_buf);
    CoTaskMemFree(pidl);

    std::string local_dir = selected_buf;
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
            // Search results have absolute paths; normal listings have relative names
            std::string remote;
            if (!e.name.empty() && e.name[0] == '/') {
                remote = e.name;
            } else {
                remote = ensure_dir_sep(remote_dir) + e.name;
            }
            // Extract basename for local filename
            std::string basename = e.name;
            auto last_sep = basename.find_last_of('/');
            if (last_sep != std::string::npos) basename = basename.substr(last_sep + 1);
            std::string local = local_dir + "\\" + basename;
            std::string pull_output;
            bool pulled = g_on_pull(remote, local, &pull_output);
            if (pulled && !shell_output_has_error(pull_output)) {
                ok++;
            } else {
                fail++;
                if (!errors.empty()) errors += "\n";
                std::string reason = pull_output.empty() ? "transfer failed" : pull_output.substr(0, 120);
                errors += "Failed: " + e.name + " (" + reason + ")";
            }
        }

        delete data;

        s_pull_error = errors;

        PostMessageW(g_hwnd, WM_USER + 202, ok, fail);
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
                        20, 44, 80, 28, hwnd, (HMENU)1001, cs->hInstance, NULL);
                    CreateWindowW(L"BUTTON", L"File",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        110, 44, 80, 28, hwnd, (HMENU)1002, cs->hInstance, NULL);
                    CreateWindowW(L"BUTTON", L"Cancel",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        200, 44, 80, 28, hwnd, (HMENU)1003, cs->hInstance, NULL);

                    HFONT hf = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                    SendMessageW(hwnd, WM_SETFONT, (WPARAM)hf, TRUE);
                    EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
                        SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE;
                    }, (LPARAM)hf);
                    SetPropW(hwnd, L"ni_font", (HANDLE)hf);
                    return 0;
                }
                case WM_COMMAND:
                    if (LOWORD(wp) == 1001) { *nd->result = 1; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
                    else if (LOWORD(wp) == 1002) { *nd->result = 2; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
                    else if (LOWORD(wp) == 1003) { *nd->result = 0; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
                    return 0;
                case WM_CLOSE:
                    DestroyWindow(hwnd);
                    return 0;
                case WM_DESTROY: {
                    HFONT hf = (HFONT)GetPropW(hwnd, L"ni_font");
                    if (hf) { DeleteObject(hf); RemovePropW(hwnd, L"ni_font"); }
                    break;
                }
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
            orr.left + (orr.right - orr.left - 320) / 2,
            orr.top + (orr.bottom - orr.top - 120) / 2,
            320, 120, g_parent, NULL, g_inst, &nd);

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

    std::vector<int> selected = get_selected_indices();
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
            // Search results have absolute paths; normal listings have relative names
            std::string remote;
            if (!e.name.empty() && e.name[0] == '/') {
                remote = e.name;
            } else {
                remote = ensure_dir_sep(dir) + e.name;
            }
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

        s_del_error = errors;

        PostMessageW(g_hwnd, WM_USER + 203, ok, fail);
        return 0;
    }, new std::pair<std::vector<FileEntry>, std::string>(std::move(to_delete), std::move(remote_dir)), 0, &tid);
}

void FileManagerWindow::copy_selected() {
    std::vector<int> selected = get_selected_indices();
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
    if (g_check_mode) exit_select_mode();
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
            // Search results have absolute paths; normal listings have relative names
            std::string src;
            if (!e.name.empty() && e.name[0] == '/') {
                src = e.name;
            } else {
                src = ensure_dir_sep(cb->source_dir) + e.name;
            }
            // Destination: always use basename + dest_dir
            std::string basename = e.name;
            auto last_sep = basename.find_last_of('/');
            if (last_sep != std::string::npos) basename = basename.substr(last_sep + 1);
            std::string dst = ensure_dir_sep(cb->dest_dir) + basename;

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

        s_paste_error = errors;

        PostMessageW(g_hwnd, WM_USER + 204, ok, fail);
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

    // Input dialog for new name — use basename for search results
    std::string basename = e.name;
    auto last_sep_pos = basename.find_last_of('/');
    if (last_sep_pos != std::string::npos) basename = basename.substr(last_sep_pos + 1);
    std::wstring value = utf8_to_wide(basename);
    if (!show_input_dialog(L"Rename", L"New name:", value)) return;
    std::string new_name = wide_to_utf8(value);

    if (new_name == basename) return;
    if (new_name.find('/') != std::string::npos || new_name.find('\\') != std::string::npos) {
        msg_box("Invalid Name", "Name cannot contain / or \\");
        return;
    }

    std::string old_path;
    if (!e.name.empty() && e.name[0] == '/') {
        old_path = e.name;
    } else {
        old_path = ensure_dir_sep(g_current_dir) + e.name;
    }
    std::string dir_of_old = old_path;
    auto sep2 = dir_of_old.find_last_of('/');
    if (sep2 != std::string::npos) dir_of_old = dir_of_old.substr(0, sep2 + 1);
    else dir_of_old = "/";
    std::string new_path = dir_of_old + new_name;

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

void FileManagerWindow::enter_select_mode() {
    if (g_check_mode) return;
    g_check_mode = true;
    // Enable checkboxes and check all items
    SendMessageW(g_file_list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
    int count = ListView_GetItemCount(g_file_list);
    SendMessageW(g_file_list, WM_SETREDRAW, FALSE, 0);
    for (int i = 0; i < count; i++) {
        ListView_SetCheckState(g_file_list, i, TRUE);
    }
    SendMessageW(g_file_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_file_list, nullptr, TRUE);
    // Switch to unchecked icon (deselect all)
    HICON hIco = load_icon_file("res\\file-manager\\file-unchecked.png", 24, 85, 85, 85, true);
    if (hIco) set_btn_icon(g_selectall_btn, hIco);
    // Disable buttons not applicable in select mode
    EnableWindow(g_import_btn, FALSE);
    EnableWindow(g_paste_btn, FALSE);
    EnableWindow(g_rename_btn, FALSE);
    EnableWindow(g_new_btn, FALSE);
    update_status();
}

void FileManagerWindow::exit_select_mode() {
    if (!g_check_mode) return;
    g_check_mode = false;
    // Remove checkboxes
    SendMessageW(g_file_list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES, 0);
    // Restore checked icon (multi-select)
    HICON hIco = load_icon_file("res\\file-manager\\file-checked.png", 24, 85, 85, 85, true);
    if (hIco) set_btn_icon(g_selectall_btn, hIco);
    // Re-enable all buttons
    EnableWindow(g_import_btn, TRUE);
    EnableWindow(g_paste_btn, TRUE);
    EnableWindow(g_rename_btn, TRUE);
    EnableWindow(g_new_btn, TRUE);
    refresh_list();
    update_status();
}

void FileManagerWindow::toggle_select_all() {
    if (!g_check_mode) return;
    int count = ListView_GetItemCount(g_file_list);
    // Check if all are currently checked
    bool all_checked = true;
    for (int i = 0; i < count; i++) {
        if (!ListView_GetCheckState(g_file_list, i)) { all_checked = false; break; }
    }
    SendMessageW(g_file_list, WM_SETREDRAW, FALSE, 0);
    if (all_checked) {
        // Uncheck all → show checked icon (select all)
        for (int i = 0; i < count; i++) ListView_SetCheckState(g_file_list, i, FALSE);
        HICON hIco = load_icon_file("res\\file-manager\\file-checked.png", 24, 85, 85, 85, true);
        if (hIco) set_btn_icon(g_selectall_btn, hIco);
    } else {
        // Check all → show unchecked icon (deselect all)
        for (int i = 0; i < count; i++) ListView_SetCheckState(g_file_list, i, TRUE);
        HICON hIco = load_icon_file("res\\file-manager\\file-unchecked.png", 24, 85, 85, 85, true);
        if (hIco) set_btn_icon(g_selectall_btn, hIco);
    }
    SendMessageW(g_file_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_file_list, nullptr, TRUE);
    update_status();
}

std::vector<int> FileManagerWindow::get_selected_indices() {
    std::vector<int> indices;
    if (g_check_mode) {
        int count = ListView_GetItemCount(g_file_list);
        for (int i = 0; i < count; i++) {
            if (ListView_GetCheckState(g_file_list, i)) {
                indices.push_back(i);
            }
        }
    } else {
        int idx = -1;
        while ((idx = ListView_GetNextItem(g_file_list, idx, LVNI_SELECTED)) >= 0) {
            indices.push_back(idx);
        }
    }
    return indices;
}

// ── Context menu ─────────────────────────────────────────

void FileManagerWindow::show_context_menu(POINT pt) {
    HMENU hMenu = CreatePopupMenu();

    int sel = ListView_GetNextItem(g_file_list, -1, LVNI_SELECTED);
    bool has_sel = (sel >= 0);

    if (has_sel && sel < (int)g_entries.size()) {
        const FileEntry &e = g_entries[sel];
        if (e.is_dir()) AppendMenuW(hMenu, MF_STRING, IDM_OPEN, L"Open");
        // "Open Containing Folder" only for search results with absolute paths
        if (!e.name.empty() && e.name[0] == '/') {
            AppendMenuW(hMenu, MF_STRING, IDM_OPEN_CONTAINING_FOLDER, L"Open Containing Folder");
        }
        AppendMenuW(hMenu, MF_STRING, IDM_PULL, L"Export to PC...");        AppendMenuW(hMenu, MF_STRING, IDM_COPY, L"Copy");
        AppendMenuW(hMenu, MF_STRING, IDM_RENAME, L"Rename");
        AppendMenuW(hMenu, MF_STRING, IDM_DELETE, L"Delete");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, IDM_PROPERTIES, L"Properties");
    } else {
        AppendMenuW(hMenu, MF_STRING, IDM_PUSH, L"Import Files from PC...");
        AppendMenuW(hMenu, MF_STRING, IDM_PUSH_FOLDER, L"Import Folder from PC...");
        AppendMenuW(hMenu, MF_STRING, IDM_NEWFOLDER, L"New Item...");
        if (!g_clipboard.entries.empty()) {
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, IDM_PASTE, L"Paste");
            if (g_clipboard.action == FileClipAction::Move) {
                AppendMenuW(hMenu, MF_STRING, IDM_MOVE_HERE, L"Move Here");
            }
        }
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        if (g_check_mode) {
            // Check if all items are checked to show appropriate menu item
            bool all_checked = true;
            int count = ListView_GetItemCount(g_file_list);
            for (int i = 0; i < count; i++) {
                if (!ListView_GetCheckState(g_file_list, i)) { all_checked = false; break; }
            }
            AppendMenuW(hMenu, MF_STRING, all_checked ? IDM_DESELECTALL : IDM_SELECTALL,
                        all_checked ? L"取消全选" : L"全选");
        } else {
            AppendMenuW(hMenu, MF_STRING, IDM_SELECTALL, L"多选");
        }
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
    // Search results have absolute paths; normal listings have relative names
    std::string path;
    if (!e.name.empty() && e.name[0] == '/') {
        path = e.name;
    } else {
        path = ensure_dir_sep(g_current_dir) + e.name;
    }

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

    show_custom_dialog(utf8_to_wide("Properties - " + e.name), utf8_to_wide(info), false);
}

// ── Status / messages ────────────────────────────────────

void FileManagerWindow::update_status() {
    if (!g_status_label) return;

    int count = (int)g_entries.size();
    std::string status = std::to_string(count) + " items";
    if (g_check_mode) {
        int checked = 0;
        for (int i = 0; i < count; i++) {
            if (ListView_GetCheckState(g_file_list, i)) checked++;
        }
        status += ", " + std::to_string(checked) + " checked";
    } else {
        int sel_count = ListView_GetSelectedCount(g_file_list);
        if (sel_count > 0) status += ", " + std::to_string(sel_count) + " selected";
    }
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
    show_custom_dialog(utf8_to_wide(title), utf8_to_wide(buf), false);
}

bool FileManagerWindow::confirm(const char *title, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return show_custom_dialog(utf8_to_wide(title), utf8_to_wide(buf), true);
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

void FileManagerWindow::show_toast(const char *text, bool success, HWND parent) {
    HWND owner = parent ? parent : g_hwnd;
    if (!owner) return;

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
    HDC hdc = GetDC(owner);
    HFONT hf = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, hf);
    std::wstring wtext = utf8_to_wide(text);
    SIZE sz;
    GetTextExtentPoint32W(hdc, wtext.c_str(), (int)wtext.size(), &sz);
    SelectObject(hdc, old);
    DeleteObject(hf);
    ReleaseDC(owner, hdc);

    int tw = sz.cx + 28;
    int th = 32;
    RECT parent_rc;
    GetClientRect(owner, &parent_rc);
    int x = (parent_rc.right - tw) / 2;
    int y = (parent_rc.bottom - th) / 2;
    POINT pt = {x, y};
    ClientToScreen(owner, &pt);
    x = pt.x;
    y = pt.y;

    g_toast_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                    L"HscrcpyToast", wtext.c_str(),
                                    WS_POPUP | WS_CLIPCHILDREN,
                                    x, y, tw, th,
                                    owner, NULL, g_inst, NULL);
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
    SendMessageW(g_file_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_file_list);

    int n = (int)g_entries.size();
    for (int i = 0; i < n; i++) {
        const FileEntry &e = g_entries[i];

        // Icon index
        int icon_idx = 2; // default: link
        if (e.is_dir()) icon_idx = 0;
        else if (e.is_file()) icon_idx = 1;

        std::wstring wname = utf8_to_wide(e.name);
        // In root directory, prefix subdirectories with '/' for clarity
        if (g_current_dir == "/" && (e.is_dir() || e.is_link()) && e.name.find('/') == std::string::npos)
            wname = L"/" + wname;
        if (e.is_dir()) wname += L"/";
        if (e.is_link()) {
            wname += L" -> " + utf8_to_wide(e.link_target);
        }

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

    SendMessageW(g_file_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_file_list, nullptr, TRUE);
}

// ── WndProc ──────────────────────────────────────────────

LRESULT CALLBACK FileManagerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // File-list icons: use ImageList_AddMasked with magenta background.
        // Color bitmap has solid tint where alpha>0, magenta where alpha=0.
        // ImageList_AddMasked auto-generates the mask from the transparent color.
        g_img_list = ImageList_Create(20, 20, ILC_COLOR32 | ILC_MASK, 3, 0);

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

            const int sz = 20;
            // Scale source
            Gdiplus::Bitmap *scaled = new Gdiplus::Bitmap(sz, sz, PixelFormat32bppARGB);
            { Gdiplus::Graphics gs(scaled);
              gs.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
              gs.Clear(Gdiplus::Color(0, 0, 0, 0));
              gs.DrawImage(src, 0, 0, sz, sz); }
            delete src;

            // Read alpha channel
            Gdiplus::BitmapData bd = {};
            Gdiplus::Rect rect(0, 0, sz, sz);
            scaled->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd);

            // Create color DIB: tint where alpha>0, magenta where alpha=0
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = sz;
            bmi.bmiHeader.biHeight = -sz;  // top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            BYTE *color_bits = NULL;
            HBITMAP hbmColor = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void **)&color_bits, NULL, 0);

            if (hbmColor && color_bits) {
                for (int y = 0; y < sz; y++) {
                    const BYTE *sp = (const BYTE *)bd.Scan0 + y * bd.Stride;
                    BYTE *dp = color_bits + y * sz * 4;
                    for (int x = 0; x < sz; x++) {
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
        add_il_icon("res\\file-manager\\filelist-folder.png", 85, 85, 85);
        add_il_icon("res\\file-manager\\filelist-file.png", 85, 85, 85);
        add_il_icon("res\\file-manager\\filelist-link.png", 85, 85, 85);

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
                                   WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_ICON,
                                   cx + 202, y1, BTN_SIZE, BTN_SIZE, hwnd, (HMENU)IDC_GO_BTN, g_inst, NULL);
        HICON hGo = load_icon_file("res\\file-manager\\go.png", 24, 85, 85, 85, true);
        if (hGo) { SendMessageW(g_go_btn, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hGo); }
        else SetWindowTextW(g_go_btn, L"Go");

        g_back_btn = CreateWindowW(L"BUTTON", L"",
                                     WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_ICON,
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
                                       WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_ICON,
                                       cx + 362, y1, BTN_SIZE, BTN_SIZE, hwnd, (HMENU)IDC_SEARCH_BTN, g_inst, NULL);
        HICON hSearch = load_icon_file("res\\file-manager\\folder-search.png", 24, 85, 85, 85, true);
        if (hSearch) { SendMessageW(g_search_btn, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hSearch); }
        else SetWindowTextW(g_search_btn, L"\U0001F50D");

        // Row 2: Toolbar buttons with icons
        int y2 = y1 + BTN_SIZE + 4;
        int bx = MARGIN;

        auto make_icon_btn = [&](CtrlId id, const char *icon_path, const wchar_t *fallback) -> HWND {
            HWND btn = CreateWindowW(L"BUTTON", L"",
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_ICON,
                                      bx, y2, BTN_SIZE, BTN_SIZE, hwnd, (HMENU)id, g_inst, NULL);
            HICON hIco = icon_path ? load_icon_file(icon_path, 24, 85, 85, 85, true) : nullptr;
            if (hIco) { SendMessageW(btn, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hIco); }
            else SetWindowTextW(btn, fallback);
            bx += BTN_SIZE + 2;
            return btn;
        };

        g_selectall_btn = make_icon_btn(IDC_SELECTALL_BTN, "res\\file-manager\\file-checked.png", L"Sel");
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
                         g_copy_btn, g_paste_btn, g_rename_btn, g_selectall_btn}) {
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
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        if (dis->CtlType == ODT_BUTTON) {
            // White face (borderless — icon glyphs self-delineate, matching
            // the main toolbar's owner-draw image buttons).
            FillRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(WHITE_BRUSH));
            HICON icon = (HICON)SendMessageW(dis->hwndItem, BM_GETIMAGE, IMAGE_ICON, 0);
            int ox = 2, oy = 2;
            if (dis->itemState & ODS_SELECTED) { ox++; oy++; }
            if (icon) {
                UINT flags = DST_ICON | ((dis->itemState & ODS_DISABLED) ? DSS_DISABLED : DSS_NORMAL);
                DrawState(dis->hDC, NULL, NULL, (LPARAM)icon, 0,
                          dis->rcItem.left + ox, dis->rcItem.top + oy, 0, 0, flags);
            } else {
                // Text fallback (icon failed to load).
                wchar_t buf[32] = {};
                GetWindowTextW(dis->hwndItem, buf, 32);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, (dis->itemState & ODS_DISABLED) ? RGB(160, 160, 160) : RGB(0, 0, 0));
                RECT tr = dis->rcItem;
                if (dis->itemState & ODS_SELECTED) { tr.left++; tr.top++; }
                DrawTextW(dis->hDC, buf, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
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
                if (lvkd->wVKey == VK_ESCAPE && g_check_mode) {
                    exit_select_mode();
                } else if (lvkd->wVKey == VK_DELETE) {
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
        case IDC_BACK_BTN: go_back(); break;
        case IDC_SEARCH_BTN: search_files(); break;
        case IDC_IMPORT_BTN: import_files(); break;
        case IDC_EXPORT_BTN: export_files(); break;
        case IDC_NEW_BTN: new_item(); break;
        case IDC_DELETE_BTN: delete_selected(); break;
        case IDC_COPY_BTN: copy_selected(); break;
        case IDC_PASTE_BTN: paste_clipboard(); break;
        case IDC_RENAME_BTN: rename_selected(); break;
        case IDC_SELECTALL_BTN:
            if (g_check_mode) toggle_select_all();
            else enter_select_mode();
            break;
        // Context menu commands
        case IDM_OPEN: enter_selected(); break;
        case IDM_OPEN_CONTAINING_FOLDER: {
            int sel = ListView_GetNextItem(g_file_list, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < (int)g_entries.size()) {
                const FileEntry &e = g_entries[sel];
                if (!e.name.empty() && e.name[0] == '/') {
                    auto last_sep = e.name.find_last_of('/');
                    std::string parent = (last_sep != std::string::npos) ? e.name.substr(0, last_sep + 1) : "/";
                    navigate(parent);
                }
            }
            break;
        }
        case IDM_PUSH: import_files(); break;
        case IDM_PUSH_FOLDER: import_folder(); break;
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
        case IDM_SELECTALL:
            if (g_check_mode) {
                // Check all items
                int count = ListView_GetItemCount(g_file_list);
                SendMessageW(g_file_list, WM_SETREDRAW, FALSE, 0);
                for (int i = 0; i < count; i++) ListView_SetCheckState(g_file_list, i, TRUE);
                SendMessageW(g_file_list, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(g_file_list, nullptr, TRUE);
                HICON hIco = load_icon_file("res\\file-manager\\file-unchecked.png", 24, 85, 85, 85, true);
                if (hIco) set_btn_icon(g_selectall_btn, hIco);
                update_status();
            } else {
                enter_select_mode();
            }
            break;
        case IDM_DESELECTALL: {
            // Uncheck all items
            int count = ListView_GetItemCount(g_file_list);
            SendMessageW(g_file_list, WM_SETREDRAW, FALSE, 0);
            for (int i = 0; i < count; i++) ListView_SetCheckState(g_file_list, i, FALSE);
            SendMessageW(g_file_list, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(g_file_list, nullptr, TRUE);
            HICON hIco = load_icon_file("res\\file-manager\\file-checked.png", 24, 85, 85, 85, true);
            if (hIco) set_btn_icon(g_selectall_btn, hIco);
            update_status();
            break;
        }
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
            // Select file requested by symlink navigation
            if (!g_select_after_refresh.empty()) {
                std::string sel_name = std::move(g_select_after_refresh);
                g_select_after_refresh.clear();
                // Search by entry name (not display text, which may have " -> target" suffix)
                int sel_idx = -1;
                for (int i = 0; i < (int)g_entries.size(); i++) {
                    if (g_entries[i].name == sel_name) { sel_idx = i; break; }
                }
                if (sel_idx >= 0) {
                    ListView_SetItemState(g_file_list, sel_idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_EnsureVisible(g_file_list, sel_idx, FALSE);
                }
            }
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
            int n_ok = (int)wp, n_fail = (int)lp;
            if (n_fail > 0 && n_ok > 0) {
                // Partial failure: toast + detail dialog
                char buf[128];
                snprintf(buf, sizeof(buf), "Pushed %d, failed %d", n_ok, n_fail);
                show_toast(buf, false);
                msg_box("Import Result", "Pushed %d item(s), %d failed.\n\n%s",
                        n_ok, n_fail, s_last_error.c_str());
            } else if (n_fail > 0) {
                msg_box("Import Failed", "All %d item(s) failed.\n\n%s",
                        n_fail, s_last_error.c_str());
            } else if (n_ok > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Pushed %d item(s)", n_ok);
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
            int n_ok = (int)wp, n_fail = (int)lp;
            if (n_fail > 0 && n_ok > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Pulled %d, failed %d", n_ok, n_fail);
                show_toast(buf, false);
                msg_box("Export Result", "Pulled %d file(s), %d failed.\n\n%s",
                        n_ok, n_fail, s_pull_error.c_str());
            } else if (n_fail > 0) {
                msg_box("Export Failed", "All %d file(s) failed.\n\n%s",
                        n_fail, s_pull_error.c_str());
            } else if (n_ok > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Pulled %d file(s)", n_ok);
                show_toast(buf, true);
            } else {
                show_toast("Pull failed", false);
            }
        }
        update_status();
        if (g_check_mode) exit_select_mode();
        break;
    }
    // delete result
    case WM_USER + 203: {
        g_busy = false;
        if (g_on_busy) g_on_busy(false);
        {
            int n_ok = (int)wp, n_fail = (int)lp;
            if (n_fail > 0 && n_ok > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Deleted %d, failed %d", n_ok, n_fail);
                show_toast(buf, false);
                msg_box("Delete Result", "Deleted %d item(s), %d failed.\n\n%s",
                        n_ok, n_fail, s_del_error.c_str());
            } else if (n_fail > 0) {
                msg_box("Delete Failed", "All %d item(s) failed.\n\n%s",
                        n_fail, s_del_error.c_str());
            } else if (n_ok > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Deleted %d item(s)", n_ok);
                show_toast(buf, true);
            } else {
                show_toast("Delete failed", false);
            }
        }
        if (g_check_mode) exit_select_mode();
        else refresh_list();
        break;
    }
    // paste result
    case WM_USER + 204: {
        g_busy = false;
        if (g_on_busy) g_on_busy(false);
        {
            int n_ok = (int)wp, n_fail = (int)lp;
            if (n_fail > 0 && n_ok > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Pasted %d, failed %d", n_ok, n_fail);
                show_toast(buf, false);
                msg_box("Paste Result", "Pasted %d item(s), %d failed.\n\n%s",
                        n_ok, n_fail, s_paste_error.c_str());
            } else if (n_fail > 0) {
                msg_box("Paste Failed", "All %d item(s) failed.\n\n%s",
                        n_fail, s_paste_error.c_str());
            } else if (n_ok > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Pasted %d item(s)", n_ok);
                show_toast(buf, true);
            } else {
                show_toast("Paste failed", false);
            }
        }
        refresh_list();
        break;
    }
    case WM_USER + 205: {
        // File content loaded — show text viewer
        g_busy = false;
        if (g_on_busy) g_on_busy(false);
        if (g_status_label) SetWindowTextW(g_status_label, L"File Manager");
        auto *result = reinterpret_cast<TextLoadResult*>(lp);
        if (result) {
            show_text_viewer(utf8_to_wide(result->file_name), result->content,
                             result->remote_path, result->read_ok);
            delete result;
        }
        break;
    }
    case WM_USER + 210: {
        // Pull-and-open result
        g_busy = false;
        if (g_on_busy) g_on_busy(false);
        if (g_status_label) SetWindowTextW(g_status_label, L"File Manager");
        auto *result = reinterpret_cast<PullOpenResult*>(lp);
        if (result) {
            if (result->ok) {
                if (result->category == kBinary) {
                    msg_box("File Saved", "Downloaded '%s' to:\n%s", result->file_name.c_str(), result->local_path.c_str());
                } else {
                    // kImage or kOther — open with Windows default viewer
                    std::wstring wpath = utf8_to_wide(result->local_path);
                    HINSTANCE h = ShellExecuteW(NULL, L"open", wpath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    if ((intptr_t)h <= 32) {
                        msg_box("File Saved", "Downloaded '%s' to:\n%s\n(No default viewer found)", result->file_name.c_str(), result->local_path.c_str());
                    }
                }
            } else {
                msg_box("Download Failed", "Failed to download '%s'", result->file_name.c_str());
            }
            delete result;
        }
        break;
    }
    case WM_USER + 211: {
        // Text viewer save request: wp = viewer hwnd, lp = pair<local_tmp, remote_path>
        // Run push+dd on background thread to avoid freezing UI
        auto *sd = reinterpret_cast<std::pair<std::string, std::string>*>(lp);
        if (!sd) break;
        HWND viewer_hwnd = (HWND)wp;
        DWORD tid;
        CreateThread(NULL, 0, [](LPVOID param) -> DWORD {
            auto *d = reinterpret_cast<std::tuple<std::string, std::string, HWND>*>(param);
            auto &local_tmp = std::get<0>(*d);
            auto &remote_path = std::get<1>(*d);
            HWND vhwnd = std::get<2>(*d);

            // Push temp file to device
            std::string remote_tmp = "/data/local/tmp/.hscrcpy_edit_tmp";
            std::string push_output;
            bool push_ok = g_on_push(local_tmp, remote_tmp, &push_output);
            if (!push_ok) {
                DeleteFileA(local_tmp.c_str());
                delete d;
                if (IsWindow(vhwnd))
                    EnableWindow(GetDlgItem(vhwnd, TV_SAVE_BTN), TRUE);
                return 0;
            }

            // Use dd to overwrite content while preserving inode metadata.
            // dd if=A of=B opens B with O_WRONLY|O_CREAT|O_TRUNC — truncates in-place,
            // keeping the same inode (owner, group, mode, SELinux xattr all preserved).
            // NOTE: Cannot use "cat tmp > file" because the '>' redirect is intercepted
            // by the Windows shell before reaching hdc, causing a local-path error.
            std::string dd_output;
            bool dd_ok = g_on_shell("dd if=" + shell_escape(remote_tmp) + " of=" + shell_escape(remote_path), &dd_output, 15);
            if (!dd_ok) {
                g_on_shell("rm " + shell_escape(remote_tmp), nullptr, 5);
                DeleteFileA(local_tmp.c_str());
                delete d;
                if (IsWindow(vhwnd))
                    EnableWindow(GetDlgItem(vhwnd, TV_SAVE_BTN), TRUE);
                return 0;
            }

            // Cleanup
            g_on_shell("rm " + shell_escape(remote_tmp), nullptr, 5);
            DeleteFileA(local_tmp.c_str());
            delete d;

            // Mark viewer as not-dirty and show success (must post back to UI thread)
            PostMessageW(g_hwnd, WM_USER + 213, (WPARAM)vhwnd, 0);
            return 0;
        }, new std::tuple<std::string, std::string, HWND>(std::move(sd->first), std::move(sd->second), viewer_hwnd), 0, &tid);
        delete sd;
        break;
    }
    case WM_USER + 213: {
        // Text viewer save completed on background thread
        HWND viewer_hwnd = (HWND)wp;
        if (IsWindow(viewer_hwnd)) {
            SetPropW(viewer_hwnd, L"tv_dirty", (HANDLE)FALSE);
            EnableWindow(GetDlgItem(viewer_hwnd, TV_SAVE_BTN), TRUE);
        }
        show_toast("File saved", true);
        break;
    }

    case WM_DESTROY:
        g_check_mode = false;
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
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace hscrcpy
