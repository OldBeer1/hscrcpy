#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include "cli.h"
#include "screen_capture.h"
#include "hdc_wrapper.h"
#include "input_injector.h"
#include "h264_decoder.h"
#include "log.h"
#include "version.h"
#include <cmath>
#include <chrono>
#include <cstdio>
#include <memory>
#include <algorithm>

using namespace hscrcpy;

static const char *WND_CLASS = "HScrcpyWndClass";
static const char *CANVAS_CLASS = "HScrcpyCanvasClass";

static HINSTANCE g_hInst = nullptr;
static HdcWrapper *g_hdc = nullptr;
static ScreenCapture *g_capture = nullptr;
static InputInjector *g_input = nullptr;
static uint32_t g_screen_w = 0, g_screen_h = 0;
static bool g_mirroring = false;
static bool g_starting = false;
static HWND g_hwnd = nullptr;
static HWND g_combo_hwnd = nullptr;
static HWND g_btn_start_hwnd = nullptr;
static HWND g_btn_refresh_hwnd = nullptr;
static HWND g_btn_back_hwnd = nullptr;
static HWND g_btn_home_hwnd = nullptr;
static HWND g_btn_recent_hwnd = nullptr;
static HWND g_btn_volup_hwnd = nullptr;
static HWND g_btn_voldown_hwnd = nullptr;
static HWND g_btn_power_hwnd = nullptr;
static HWND g_btn_reboot_hwnd = nullptr;
static HWND g_btn_quality_hwnd = nullptr;
static HWND g_status_hwnd = nullptr;
static HWND g_canvas_hwnd = nullptr;

static std::vector<HdcDevice> g_devices;
static hscrcpy::CliArgs g_cli_args;

static std::shared_ptr<DecodedImage> g_cur_frame;
static std::mutex g_frame_mutex;
static uint64_t g_last_rendered_seq = 0;
static HANDLE g_frame_event = nullptr;

static int TOOLBAR_H = 62;
static bool g_snapshot_warned = false;

static bool g_mouse_down = false;
static uint32_t g_drag_start_x = 0, g_drag_start_y = 0;
static auto g_last_click_time = std::chrono::steady_clock::now();

static CaptureConfig g_capture_config;
static bool g_config_initialized = false;
static uint32_t g_device_screen_w = 0, g_device_screen_h = 0;

#define ID_COMBO       1001
#define ID_BTN_START   1002
#define ID_BTN_REFRESH 1003
#define ID_BTN_POWER   1004
#define ID_BTN_BACK    1005
#define ID_BTN_VOLUP   1006
#define ID_BTN_VOLDOWN 1007
#define ID_BTN_HOME    1008
#define ID_BTN_REBOOT  1009
#define ID_BTN_RECENT  1010
#define ID_BTN_QUALITY 1011

static ULONG_PTR g_gdiplus_token = 0;

// Persistent backbuffer for GDI rendering
static HDC g_backbuffer_dc = nullptr;
static HBITMAP g_backbuffer_bmp = nullptr;
static HBITMAP g_backbuffer_old = nullptr;
static int g_backbuffer_w = 0;
static int g_backbuffer_h = 0;

static void destroy_backbuffer() {
    if (g_backbuffer_dc) {
        if (g_backbuffer_old) SelectObject(g_backbuffer_dc, g_backbuffer_old);
        if (g_backbuffer_bmp) DeleteObject(g_backbuffer_bmp);
        DeleteDC(g_backbuffer_dc);
    }
    g_backbuffer_dc = nullptr;
    g_backbuffer_bmp = nullptr;
    g_backbuffer_old = nullptr;
    g_backbuffer_w = 0;
    g_backbuffer_h = 0;
}

static bool ensure_backbuffer(int w, int h) {
    if (g_backbuffer_dc && g_backbuffer_w >= w && g_backbuffer_h >= h) return true;
    destroy_backbuffer();
    HDC screen_dc = GetDC(nullptr);
    g_backbuffer_dc = CreateCompatibleDC(screen_dc);
    g_backbuffer_bmp = CreateCompatibleBitmap(screen_dc, w, h);
    ReleaseDC(nullptr, screen_dc);
    if (!g_backbuffer_dc || !g_backbuffer_bmp) {
        destroy_backbuffer();
        return false;
    }
    g_backbuffer_old = (HBITMAP)SelectObject(g_backbuffer_dc, g_backbuffer_bmp);
    g_backbuffer_w = w;
    g_backbuffer_h = h;
    return true;
}

static HICON load_png_icon(const char *relative_path, int size) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string dir(exe_path);
    auto sep = dir.find_last_of("\\/");
    if (sep != std::string::npos) dir = dir.substr(0, sep + 1);
    std::string path = dir + relative_path;

    HICON icon = NULL;
    Gdiplus::Bitmap *src = Gdiplus::Bitmap::FromFile(std::wstring(path.begin(), path.end()).c_str());
    if (src && src->GetLastStatus() == Gdiplus::Ok) {
        int sw = src->GetWidth(), sh = src->GetHeight();
        int dw = size, dh = size;
        if (sw * dh > sh * dw) {
            dh = (int)(size * (double)sh / sw);
        } else {
            dw = (int)(size * (double)sw / sh);
        }
        int ox = (size - dw) / 2, oy = (size - dh) / 2;

        Gdiplus::Bitmap *dst = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
        Gdiplus::Graphics g(dst);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.DrawImage(src, ox, oy, dw, dh);
        dst->GetHICON(&icon);
        delete dst;
        delete src;
    }
    return icon;
}

static HHOOK g_msgbox_hook = NULL;

static LRESULT CALLBACK MsgBoxCBTProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HCBT_ACTIVATE) {
        HWND hMsg = (HWND)wp;
        if (g_hwnd) {
            RECT pr, mr;
            GetWindowRect(g_hwnd, &pr);
            GetWindowRect(hMsg, &mr);
            int x = pr.left + (pr.right - pr.left - (mr.right - mr.left)) / 2;
            int y = pr.top + (pr.bottom - pr.top - (mr.bottom - mr.top)) / 2;
            SetWindowPos(hMsg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        UnhookWindowsHookEx(g_msgbox_hook);
        g_msgbox_hook = NULL;
    }
    return CallNextHookEx(g_msgbox_hook, nCode, wp, lp);
}

static int center_msgbox(const char *text, const char *title, UINT type) {
    g_msgbox_hook = SetWindowsHookExA(WH_CBT, MsgBoxCBTProc, NULL, GetCurrentThreadId());
    int ret = MessageBoxA(g_hwnd, text, title, type);
    if (g_msgbox_hook) {
        UnhookWindowsHookEx(g_msgbox_hook);
        g_msgbox_hook = NULL;
    }
    return ret;
}

static void show_error(const char *msg) {
    center_msgbox(msg, "HScrcpy Error", MB_OK | MB_ICONERROR);
}

static void set_status(const char *msg) {
    if (g_status_hwnd) SetWindowTextA(g_status_hwnd, msg);
}

static const char *bitrate_label(int bitrate) {
    if (bitrate >= 1000000) {
        static char buf[32];
        snprintf(buf, sizeof(buf), "%dM", bitrate / 1000000);
        return buf;
    }
    if (bitrate >= 1000) {
        static char buf[32];
        snprintf(buf, sizeof(buf), "%dK", bitrate / 1000);
        return buf;
    }
    return "?";
}

static void query_device_resolution() {
    if (!g_hdc) return;
    uint32_t w = 0, h = 0;
    bool got = g_hdc->get_screen_resolution(w, h);
    if (got && w > 0) {
        g_device_screen_w = w;
        g_device_screen_h = h;
        LOGI("Device physical screen: %ux%u (tcp=%s)", w, h, g_hdc->is_tcp() ? "yes" : "no");
    } else {
        LOGW("Failed to get device screen resolution, using defaults");
        g_device_screen_w = 0;
        g_device_screen_h = 0;
    }
}

static void auto_select_config() {
    query_device_resolution();

    g_capture_config = CaptureConfig{};
    bool is_tcp = g_hdc && g_hdc->is_tcp();

    uint32_t eff_w = g_device_screen_w > 0 ? g_device_screen_w : 1260;
    if (eff_w > 720) {
        g_capture_config.scale = 2;
    } else {
        g_capture_config.scale = 1;
    }

    if (is_tcp) {
        g_capture_config.bit_rate = 4194304;
        g_capture_config.frame_rate = 24;
        LOGI("Auto TCP: %ux%u -> scale=%d, 4M, 24fps", eff_w, g_device_screen_h, g_capture_config.scale);
    } else {
        g_capture_config.bit_rate = 8388608;
        g_capture_config.frame_rate = 30;
        LOGI("Auto USB: %ux%u -> scale=%d, 8M, 30fps", eff_w, g_device_screen_h, g_capture_config.scale);
    }
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        RECT pr, dr;
        GetWindowRect(g_hwnd, &pr);
        GetWindowRect(hDlg, &dr);
        int x = pr.left + (pr.right - pr.left - (dr.right - dr.left)) / 2;
        int y = pr.top + (pr.bottom - pr.top - (dr.bottom - dr.top)) / 2;
        SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        HWND hScale = GetDlgItem(hDlg, 100);
        HWND hBitrate = GetDlgItem(hDlg, 101);
        HWND hFps = GetDlgItem(hDlg, 102);

        uint32_t dw = g_device_screen_w > 0 ? g_device_screen_w : 1260;
        uint32_t dh = g_device_screen_h > 0 ? g_device_screen_h : 2720;

        char label[64];
        for (int s = 1; s <= 5; s++) {
            snprintf(label, sizeof(label), "%d  (%ux%u)", s, dw / s, dh / s);
            int idx = (int)SendMessageA(hScale, CB_ADDSTRING, 0, (LPARAM)label);
            SendMessageA(hScale, CB_SETITEMDATA, idx, s);
        }
        int sel_scale = g_capture_config.scale - 1;
        SendMessageA(hScale, CB_SETCURSEL, sel_scale, 0);

        struct BitrateEntry { const char *label; int value; };
        BitrateEntry bitrates[] = {
            {"30 Mbps (USB Original)", 31457280},
            {"20 Mbps (USB 2K Scaled)", 20971520},
            {"8 Mbps (WiFi Balanced)", 8388608},
            {"4 Mbps (WiFi Low BW)", 4194304},
            {"2 Mbps (Remote TCP)", 2097152},
        };
        int sel_br = 0;
        for (int i = 0; i < 5; i++) {
            int idx = (int)SendMessageA(hBitrate, CB_ADDSTRING, 0, (LPARAM)bitrates[i].label);
            SendMessageA(hBitrate, CB_SETITEMDATA, idx, bitrates[i].value);
            if (bitrates[i].value == g_capture_config.bit_rate) sel_br = i;
        }
        SendMessageA(hBitrate, CB_SETCURSEL, sel_br, 0);

        struct FpsEntry { const char *label; int value; };
        FpsEntry fpses[] = {
            {"60 fps", 60},
            {"30 fps", 30},
            {"24 fps", 24},
            {"15 fps", 15},
            {"10 fps", 10},
        };
        int sel_fps = 1;
        for (int i = 0; i < 5; i++) {
            int idx = (int)SendMessageA(hFps, CB_ADDSTRING, 0, (LPARAM)fpses[i].label);
            SendMessageA(hFps, CB_SETITEMDATA, idx, fpses[i].value);
            if (fpses[i].value == g_capture_config.frame_rate) sel_fps = i;
        }
        SendMessageA(hFps, CB_SETCURSEL, sel_fps, 0);

        char info[128];
        snprintf(info, sizeof(info), "Device: %ux%u  |  Current: scale=%d, %s, %dfps",
                 dw, dh, g_capture_config.scale,
                 bitrate_label(g_capture_config.bit_rate), g_capture_config.frame_rate);
        SetWindowTextA(GetDlgItem(hDlg, 103), info);

        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            HWND hScale = GetDlgItem(hDlg, 100);
            HWND hBitrate = GetDlgItem(hDlg, 101);
            HWND hFps = GetDlgItem(hDlg, 102);

            int s = (int)SendMessageA(hScale, CB_GETCURSEL, 0, 0);
            int scale_val = (int)SendMessageA(hScale, CB_GETITEMDATA, s, 0);
            int b = (int)SendMessageA(hBitrate, CB_GETCURSEL, 0, 0);
            int br_val = (int)SendMessageA(hBitrate, CB_GETITEMDATA, b, 0);
            int f = (int)SendMessageA(hFps, CB_GETCURSEL, 0, 0);
            int fps_val = (int)SendMessageA(hFps, CB_GETITEMDATA, f, 0);

            CaptureConfig old_config = g_capture_config;
            g_capture_config.scale = scale_val;
            g_capture_config.bit_rate = br_val;
            g_capture_config.frame_rate = fps_val;

            LOGI("User set: scale=%d, bitrate=%d, fps=%d", scale_val, br_val, fps_val);
            EndDialog(hDlg, IDOK);

            if (g_mirroring && (old_config.scale != g_capture_config.scale ||
                                old_config.bit_rate != g_capture_config.bit_rate ||
                                old_config.frame_rate != g_capture_config.frame_rate)) {
                if (center_msgbox("Quality settings changed. Restart mirroring to apply?",
                                  "HScrcpy", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    PostMessageA(g_hwnd, WM_USER + 7, 0, 0);
                }
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void show_settings_dialog() {
    struct {
        DLGTEMPLATE dlg;
        WORD menu, cls, title;
    } tmpl = {};
    tmpl.dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_MODALFRAME;
    tmpl.dlg.cx = 240;
    tmpl.dlg.cy = 140;
    tmpl.dlg.cdit = 8;
    tmpl.menu = 0;
    tmpl.cls = 0;
    tmpl.title = 0;

    struct Item {
        DLGITEMTEMPLATE item;
        WORD cls;
        WORD id_lo;
        WORD id_hi;
        WORD text;
    };

    static std::vector<BYTE> data;
    data.resize(4096);
    BYTE *p = data.data();
    auto align = [&]() { p += (4 - ((ULONG_PTR)p % 4)) % 4; };
    auto write = [&](const void *src, size_t len) { memcpy(p, src, len); p += len; };

    write(&tmpl.dlg, sizeof(tmpl.dlg));
    p += sizeof(WORD) * 3;

    auto writeItem = [&](DWORD style, WORD x, WORD y, WORD cx, WORD cy, WORD id, const wchar_t *cls, const wchar_t *text) {
        align();
        DLGITEMTEMPLATE item = {};
        item.style = style;
        item.x = x; item.y = y; item.cx = cx; item.cy = cy;
        item.id = id;
        write(&item, sizeof(item));
        size_t clsLen = (wcslen(cls) + 1) * sizeof(wchar_t);
        write(cls, clsLen);
        size_t txtLen = (wcslen(text) + 1) * sizeof(wchar_t);
        write(text, txtLen);
        p += sizeof(WORD);
    };

    writeItem(WS_CHILD | WS_VISIBLE | SS_LEFT, 7, 7, 60, 8, 10, L"STATIC", L"Scale:");
    writeItem(WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 70, 5, 150, 200, 100, L"COMBOBOX", L"");
    writeItem(WS_CHILD | WS_VISIBLE | SS_LEFT, 7, 27, 60, 8, 11, L"STATIC", L"Bitrate:");
    writeItem(WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 70, 25, 150, 200, 101, L"COMBOBOX", L"");
    writeItem(WS_CHILD | WS_VISIBLE | SS_LEFT, 7, 47, 60, 8, 12, L"STATIC", L"Frame Rate:");
    writeItem(WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 70, 45, 150, 200, 102, L"COMBOBOX", L"");
    writeItem(WS_CHILD | WS_VISIBLE | SS_LEFT, 7, 70, 213, 10, 103, L"STATIC", L"");
    writeItem(WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 120, 90, 50, 14, IDOK, L"BUTTON", L"OK");
    writeItem(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 175, 90, 50, 14, IDCANCEL, L"BUTTON", L"Cancel");

    tmpl.dlg.cdit = 8;
    memcpy(data.data(), &tmpl.dlg, sizeof(tmpl.dlg));

    DialogBoxIndirectParamA(g_hInst, (DLGTEMPLATE *)data.data(), g_hwnd, SettingsDlgProc, 0);
}

static void refresh_devices() {
    std::string current_serial;
    if (g_mirroring && g_hdc) {
        current_serial = g_hdc->serial();
    } else {
        int sel = (int)SendMessageA(g_combo_hwnd, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR && sel < (int)g_devices.size()) {
            current_serial = g_devices[sel].serial;
        }
    }

    SetWindowTextA(g_status_hwnd, "Scanning...");
    SendMessageA(g_combo_hwnd, CB_RESETCONTENT, 0, 0);

    HdcWrapper hdc_local;
    g_devices = hdc_local.list_devices();

    int select_idx = 0;
    int cur = 0;
    for (auto &d : g_devices) {
        std::string entry = d.serial;
        SendMessageA(g_combo_hwnd, CB_ADDSTRING, 0, (LPARAM)entry.c_str());
        if (!current_serial.empty() && d.serial == current_serial) {
            select_idx = cur;
        }
        cur++;
    }
    if (!g_devices.empty()) {
        SendMessageA(g_combo_hwnd, CB_SETCURSEL, select_idx, 0);
        EnableWindow(g_btn_start_hwnd, TRUE);
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "%d device(s)", (int)g_devices.size());
    SetWindowTextA(g_status_hwnd, buf);
}

static std::string get_selected_serial() {
    int sel = (int)SendMessageA(g_combo_hwnd, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR || sel >= (int)g_devices.size()) return "";
    return g_devices[sel].serial;
}

static HdcDevice get_selected_device() {
    int sel = (int)SendMessageA(g_combo_hwnd, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR || sel >= (int)g_devices.size()) return {};
    return g_devices[sel];
}

static std::pair<uint32_t, uint32_t> canvas_to_screen(int wx, int wy) {
    if (g_screen_w == 0 || g_screen_h == 0) return {0, 0};
    RECT rc;
    GetClientRect(g_canvas_hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return {0, 0};

    double aspect = (double)g_screen_w / g_screen_h;
    int draw_w, draw_h;
    if ((double)cw / ch > aspect) {
        draw_h = ch;
        draw_w = (int)(ch * aspect);
    } else {
        draw_w = cw;
        draw_h = (int)(cw / aspect);
    }
    int off_x = (cw - draw_w) / 2;
    int off_y = (ch - draw_h) / 2;

    int rel_x = wx - off_x;
    int rel_y = wy - off_y;
    if (rel_x < 0) rel_x = 0;
    if (rel_y < 0) rel_y = 0;
    if (rel_x > draw_w) rel_x = draw_w;
    if (rel_y > draw_h) rel_y = draw_h;

    uint32_t phy_w = g_screen_w * g_capture_config.scale;
    uint32_t phy_h = g_screen_h * g_capture_config.scale;

    return {static_cast<uint32_t>(rel_x * phy_w / draw_w),
            static_cast<uint32_t>(rel_y * phy_h / draw_h)};
}

static WNDPROC g_canvas_orig_wndproc = nullptr;

static int windows_key_to_harmonyos(int vk) {
    switch (vk) {
    case VK_BACK:   return 2055;
    case VK_TAB:    return 2049;
    case VK_RETURN: return 2119;
    case VK_ESCAPE: return 2070;
    case VK_SPACE:  return 2050;
    case VK_LEFT:   return 2014;
    case VK_UP:     return 2012;
    case VK_RIGHT:  return 2015;
    case VK_DOWN:   return 2013;
    case VK_INSERT: return 2057;
    case VK_DELETE: return 2056;
    default:        return -1;
    }
}

static uint32_t g_last_touch_x = 0, g_last_touch_y = 0;

static std::string g_cjk_buf;
static uint64_t g_cjk_buf_time = 0;
static uint32_t g_cjk_x = 0, g_cjk_y = 0;

static void flush_cjk_buffer() {
    std::string text;
    uint32_t x, y;
    {
        text = std::move(g_cjk_buf);
        g_cjk_buf.clear();
        g_cjk_buf_time = 0;
        x = g_cjk_x;
        y = g_cjk_y;
    }
    if (!text.empty() && g_input) {
        LOGI("CJK flush: '%s' at (%u, %u)", text.c_str(), x, y);
        g_input->inject_text(text, x, y);
    }
}

static LRESULT CALLBACK CanvasKeyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_mirroring && g_input) {
        if (msg == WM_KEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            if (ctrl && wp == 'V') {
                if (OpenClipboard(hwnd)) {
                    HANDLE h = GetClipboardData(CF_UNICODETEXT);
                    if (h) {
                        wchar_t *wtext = static_cast<wchar_t *>(GlobalLock(h));
                        if (wtext) {
                            int glen = WideCharToMultiByte(CP_ACP, 0, wtext, -1, NULL, 0, NULL, NULL);
                            if (glen > 0) {
                                std::vector<char> buf(glen);
                                WideCharToMultiByte(CP_ACP, 0, wtext, -1, buf.data(), glen, NULL, NULL);
                                std::string s(buf.data());
                                if (!s.empty()) {
                                    LOGI("Ctrl+V paste: '%s' at (%u, %u)", s.c_str(), g_last_touch_x, g_last_touch_y);
                                    g_input->inject_text(s, g_last_touch_x, g_last_touch_y);
                                }
                            }
                            GlobalUnlock(h);
                        }
                    } else {
                        HANDLE h2 = GetClipboardData(CF_TEXT);
                        if (h2) {
                            char *text = static_cast<char *>(GlobalLock(h2));
                            if (text) {
                                std::string s(text);
                                GlobalUnlock(h2);
                                if (!s.empty()) {
                                    LOGI("Ctrl+V paste (ANSI): '%s' at (%u, %u)", s.c_str(), g_last_touch_x, g_last_touch_y);
                                    g_input->inject_text(s, g_last_touch_x, g_last_touch_y);
                                }
                            }
                        }
                    }
                    CloseClipboard();
                }
                return 0;
            }
            if (ctrl) return CallWindowProcA(g_canvas_orig_wndproc, hwnd, msg, wp, lp);

            if (wp >= 'A' && wp <= 'Z') {
                int code = (wp - 'A') + 2017;
                LOGI("Key '%c' -> HarmonyOS %d shift=%d", (char)wp, code, shift);
                g_input->inject_key_code(code, shift);
                return 0;
            }
            if (wp >= '0' && wp <= '9') {
                int code = (wp - '0') + 2000;
                LOGI("Key '%c' -> HarmonyOS %d", (char)wp, code);
                g_input->inject_key_code(code);
                return 0;
            }
            int hk = windows_key_to_harmonyos(static_cast<int>(wp));
            if (hk > 0) {
                LOGI("Key vk=%d -> HarmonyOS %d", (int)wp, hk);
                g_input->inject_key_code(hk);
                return 0;
            }
        }
        if (msg == WM_CHAR) {
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                return CallWindowProcA(g_canvas_orig_wndproc, hwnd, msg, wp, lp);
            }
            char ch = static_cast<char>(wp & 0xFF);
            if (ch >= 32 && ch < 127) {
                return 0;
            }
            if (static_cast<unsigned char>(ch) >= 0x80) {
                static std::string pending_mb;
                pending_mb += ch;
                if (pending_mb.size() >= 2) {
                    int wlen = MultiByteToWideChar(CP_ACP, 0, pending_mb.c_str(), (int)pending_mb.size(), NULL, 0);
                    if (wlen > 0) {
                        std::vector<wchar_t> wbuf(wlen);
                        MultiByteToWideChar(CP_ACP, 0, pending_mb.c_str(), (int)pending_mb.size(), wbuf.data(), wlen);
                        char gbk[64];
                        int glen = WideCharToMultiByte(CP_ACP, 0, wbuf.data(), wlen, gbk, sizeof(gbk), NULL, NULL);
                        if (glen > 0) {
                            g_cjk_buf.append(gbk, glen);
                            g_cjk_buf_time = GetTickCount64();
                            g_cjk_x = g_last_touch_x;
                            g_cjk_y = g_last_touch_y;
                        }
                    }
                    pending_mb.clear();
                }
                return 0;
            }
        }
    }
    return CallWindowProcA(g_canvas_orig_wndproc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK CanvasWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int cw = rc.right, ch = rc.bottom;

        if (ensure_backbuffer(cw, ch)) {
            HBRUSH bg = CreateSolidBrush(RGB(30, 30, 30));
            FillRect(g_backbuffer_dc, &rc, bg);
            DeleteObject(bg);

            {
                std::lock_guard<std::mutex> lock(g_frame_mutex);
                if (g_cur_frame && !g_cur_frame->data.empty() && g_cur_frame->width > 0) {
                    double aspect = (double)g_cur_frame->width / g_cur_frame->height;
                    int draw_w, draw_h, off_x, off_y;
                    if ((double)cw / ch > aspect) {
                        draw_h = ch;
                        draw_w = (int)(ch * aspect);
                    } else {
                        draw_w = cw;
                        draw_h = (int)(cw / aspect);
                    }
                    off_x = (cw - draw_w) / 2;
                    off_y = (ch - draw_h) / 2;

                    BITMAPINFOHEADER bih = {};
                    bih.biSize     = sizeof(bih);
                    bih.biWidth    = static_cast<LONG>(g_cur_frame->width);
                    bih.biHeight   = -static_cast<LONG>(g_cur_frame->height);
                    bih.biPlanes   = 1;
                    bih.biBitCount = 24;
                    bih.biCompression = BI_RGB;
                    bih.biSizeImage = g_cur_frame->data.size();

                    SetStretchBltMode(g_backbuffer_dc, COLORONCOLOR);
                    StretchDIBits(g_backbuffer_dc, off_x, off_y, draw_w, draw_h,
                                  0, 0, g_cur_frame->width, g_cur_frame->height,
                                  g_cur_frame->data.data(),
                                  reinterpret_cast<BITMAPINFO *>(&bih),
                                  DIB_RGB_COLORS, SRCCOPY);
                }
            }

            BitBlt(hdc, 0, 0, cw, ch, g_backbuffer_dc, 0, 0, SRCCOPY);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        LOGI("Canvas WM_LBUTTONDOWN");
        if (g_mirroring && g_input) {
            HWND prev = SetFocus(hwnd);
            LOGI("SetFocus to canvas: hwnd=%p, prev=%p, err=%d", hwnd, prev, GetLastError());
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            auto [x, y] = canvas_to_screen(pt.x, pt.y);
            g_mouse_down = true;
            g_drag_start_x = x; g_drag_start_y = y;
            g_last_touch_x = x; g_last_touch_y = y;
            g_last_click_time = std::chrono::steady_clock::now();
            g_input->queue_touch_down(x, y);
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_mouse_down && g_mirroring && g_input) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            auto [x, y] = canvas_to_screen(pt.x, pt.y);
            int dx = (int)x - (int)g_last_touch_x;
            int dy = (int)y - (int)g_last_touch_y;
            if (dx * dx + dy * dy >= 16) {
                g_last_touch_x = x;
                g_last_touch_y = y;
                g_input->queue_touch_move(x, y);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_mouse_down && g_mirroring && g_input) {
            g_mouse_down = false;
            ReleaseCapture();
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            auto [x, y] = canvas_to_screen(pt.x, pt.y);
            auto elapsed = std::chrono::steady_clock::now() - g_last_click_time;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            int dx = static_cast<int>(x) - static_cast<int>(g_drag_start_x);
            int dy = static_cast<int>(y) - static_cast<int>(g_drag_start_y);
            if (dx * dx + dy * dy >= 100) {
                g_input->queue_touch_up(x, y);
            } else if (ms > 500) {
                g_input->queue_touch_up(x, y);
                g_input->inject_long_click(x, y);
            } else {
                g_input->queue_touch_up(x, y);
            }
        }
        return 0;
    }
    case WM_RBUTTONUP:
        if (g_mirroring && g_input) g_input->inject_back();
        return 0;
    case WM_MOUSEWHEEL: {
        if (g_mirroring && g_input) {
            auto delta = GET_WHEEL_DELTA_WPARAM(wp);
            POINTS pts = MAKEPOINTS(lp);
            POINT pt = {pts.x, pts.y};
            auto [x, y] = canvas_to_screen(pt.x, pt.y);
            g_input->inject_wheel(x, y, delta);
        }
        return 0;
    }
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

static uint64_t g_last_drawn_frame = 0;
static uint64_t g_fps_counter = 0;
static DWORD g_fps_start_tick = 0;
static DWORD g_last_frame_tick = 0;

// Track background threads to avoid detach
static std::thread g_cleanup_thread;
static std::thread g_startup_thread;

static void join_cleanup_thread() {
    if (g_cleanup_thread.joinable()) g_cleanup_thread.join();
}
static void join_startup_thread() {
    if (g_startup_thread.joinable()) g_startup_thread.join();
}

static void stop_mirroring();

static void try_update_frame() {
    if (!g_capture) return;

    auto fc = g_capture->frame_count();
    if (g_mirroring && fc != g_last_rendered_seq) {
        g_last_frame_tick = GetTickCount();
    }
    if (g_mirroring && g_last_frame_tick > 0 && GetTickCount() - g_last_frame_tick > 5000) {
        LOGW("No frame for 5s, device disconnected");
        set_status("HDC disconnected");
        stop_mirroring();
        return;
    }

    if (fc != g_last_rendered_seq) {
        std::shared_ptr<DecodedImage> img;
        if (g_capture->get_latest_frame(img) && img && !img->data.empty()) {
            if (img->width != g_screen_w || img->height != g_screen_h) {
                LOGI("Frame resolution changed: %ux%u -> %ux%u",
                     g_screen_w, g_screen_h, img->width, img->height);
                g_screen_w = img->width;
                g_screen_h = img->height;
                PostMessageA(g_hwnd, WM_USER + 5, 0, 0);
            }
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_cur_frame = std::move(img);
            g_last_rendered_seq = fc;
            InvalidateRect(g_canvas_hwnd, NULL, FALSE);

            g_fps_counter++;
            DWORD now = GetTickCount();
            if (g_fps_start_tick == 0) g_fps_start_tick = now;
            if (now - g_fps_start_tick >= 3000) {
                DWORD fps = (DWORD)(g_fps_counter * 1000 / (now - g_fps_start_tick));
                LOGI("Render FPS: %d (scale=%d, %s)", fps, g_capture_config.scale, bitrate_label(g_capture_config.bit_rate));
                char buf[64];
                snprintf(buf, sizeof(buf), "%dfps", fps);
                set_status(buf);
                g_fps_counter = 0;
                g_fps_start_tick = now;
            }
        }
    }
}

static bool g_stopping = false;
static std::string g_cleanup_serial;
static std::string g_pending_start_serial;
static std::string g_pending_start_ip;
static std::string g_pending_start_port;

static void stop_mirroring() {
    if (!g_mirroring && !g_starting) return;
    if (g_stopping) return;

    std::string stopping_serial;
    if (g_hdc) stopping_serial = g_hdc->serial();

    g_mirroring = false;
    g_starting = false;
    g_stopping = true;
    g_cleanup_serial = stopping_serial;

    g_screen_w = 0;
    g_screen_h = 0;
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        g_cur_frame.reset();
    }

    SetWindowTextA(g_btn_start_hwnd, "Start Mirroring");
    EnableWindow(g_btn_start_hwnd, TRUE);
    set_status("Stopping...");
    g_snapshot_warned = false;

    InvalidateRect(g_canvas_hwnd, NULL, TRUE);

    auto *cap = g_capture;
    auto *inp = g_input;
    auto *hdc = g_hdc;
    g_capture = nullptr;
    g_input = nullptr;
    g_hdc = nullptr;

    join_cleanup_thread();
    g_cleanup_thread = std::thread([cap, inp, hdc]() {
        if (cap) { cap->stop(); delete cap; }
        if (inp) { delete inp; }
        if (hdc) { delete hdc; }
        PostMessageA(g_hwnd, WM_USER + 4, 0, 0);
    });
}

static void start_mirroring(const std::string &serial, const std::string &ip = "127.0.0.1", const std::string &port = "8710") {
    if (g_mirroring || g_starting) {
        return;
    }
    if (g_stopping) {
        if (!g_cleanup_serial.empty() && serial == g_cleanup_serial) {
            g_pending_start_serial = serial;
            g_pending_start_ip = ip;
            g_pending_start_port = port;
            return;
        }
    }

    g_starting = true;
    set_status("Connecting...");
    EnableWindow(g_btn_start_hwnd, FALSE);

    g_hdc = new HdcWrapper(serial, ip, port);

    join_startup_thread();
    g_startup_thread = std::thread([serial, ip, port]() {
        // Quick connectivity check: verify device is actually in hdc device list
        {
            std::vector<HdcDevice> devs = g_hdc->list_devices();
            bool found = false;
            for (const auto &d : devs) {
                if (d.serial == serial) { found = true; break; }
            }
            if (!found) {
                LOGE("Device %s not in hdc device list", serial.c_str());
                PostMessageA(g_hwnd, WM_USER + 1, 0, 0);
                return;
            }
        }
        // Secondary: quick echo to verify shell access
        if (!g_hdc->shell_command("echo ok", nullptr, 3)) {
            LOGE("Device %s not reachable", serial.c_str());
            PostMessageA(g_hwnd, WM_USER + 1, 0, 0);
            return;
        }

        if (!g_config_initialized) {
            auto_select_config();
            g_config_initialized = true;
        } else {
            query_device_resolution();
        }

        if (g_cli_args.scale > 0) {
            g_capture_config.scale = g_cli_args.scale;
            LOGI("CLI override: scale=%d", g_cli_args.scale);
        }
        if (g_cli_args.bitrate > 0) {
            g_capture_config.bit_rate = g_cli_args.bitrate;
            LOGI("CLI override: bitrate=%d", g_cli_args.bitrate);
        }
        if (g_cli_args.fps != kDefaultFps) {
            g_capture_config.frame_rate = g_cli_args.fps;
            LOGI("CLI override: fps=%u", g_cli_args.fps);
        }

        g_capture = new ScreenCapture(*g_hdc, g_capture_config);

        char status_buf[256];
        snprintf(status_buf, sizeof(status_buf), "Connecting... (scale=%d, %s, %dfps)",
                 g_capture_config.scale, bitrate_label(g_capture_config.bit_rate), g_capture_config.frame_rate);
        PostMessageA(g_hwnd, WM_USER + 6, 0, 0);

        bool ok = false;
        try {
            ok = g_capture->start();
        } catch (const std::exception &e) {
            LOGE("g_capture->start() exception: %s", e.what());
            ok = false;
        }

        if (!ok) {
            PostMessageA(g_hwnd, WM_USER + 1, 0, 0);
            return;
        }

        HANDLE evt = g_capture->frame_event();
        for (int i = 0; i < 150 && g_capture->width() == 0; i++) {
            if (evt) {
                WaitForSingleObject(evt, 100);
            } else {
                Sleep(100);
            }
        }

        if (g_capture->width() == 0) {
            PostMessageA(g_hwnd, WM_USER + 2, 0, 0);
            return;
        }

        PostMessageA(g_hwnd, WM_USER + 3, 0, 0);
    });
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowA("STATIC", "Device:",
                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                       8, 8, 48, 18, hwnd, NULL, g_hInst, NULL);

        g_combo_hwnd = CreateWindowA("COMBOBOX", "",
                                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      58, 5, 180, 200, hwnd, (HMENU)ID_COMBO, g_hInst, NULL);

        g_btn_refresh_hwnd = CreateWindowA("BUTTON", "Refresh",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                            245, 5, 52, 22, hwnd, (HMENU)ID_BTN_REFRESH, g_hInst, NULL);

        g_btn_start_hwnd = CreateWindowA("BUTTON", "Start",
                                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                                           301, 5, 100, 22, hwnd, (HMENU)ID_BTN_START, g_hInst, NULL);

        g_btn_back_hwnd = CreateWindowA("BUTTON", "Back",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         8, 34, 48, 22, hwnd, (HMENU)ID_BTN_BACK, g_hInst, NULL);
        g_btn_home_hwnd = CreateWindowA("BUTTON", "Home",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         64, 34, 48, 22, hwnd, (HMENU)ID_BTN_HOME, g_hInst, NULL);
        g_btn_recent_hwnd = CreateWindowA("BUTTON", "Recent",
                                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                           116, 34, 50, 22, hwnd, (HMENU)ID_BTN_RECENT, g_hInst, NULL);
        g_btn_volup_hwnd = CreateWindowA("BUTTON", "Vol+",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          170, 34, 42, 22, hwnd, (HMENU)ID_BTN_VOLUP, g_hInst, NULL);
        g_btn_voldown_hwnd = CreateWindowA("BUTTON", "Vol-",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                            216, 34, 42, 22, hwnd, (HMENU)ID_BTN_VOLDOWN, g_hInst, NULL);
        g_btn_power_hwnd = CreateWindowA("BUTTON", "Power",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          262, 34, 48, 22, hwnd, (HMENU)ID_BTN_POWER, g_hInst, NULL);
        g_btn_reboot_hwnd = CreateWindowA("BUTTON", "Reboot",
                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                             314, 34, 50, 22, hwnd, (HMENU)ID_BTN_REBOOT, g_hInst, NULL);

        g_btn_quality_hwnd = CreateWindowA("BUTTON", "",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_ICON,
                                            370, 34, 26, 22, hwnd, (HMENU)ID_BTN_QUALITY, g_hInst, NULL);
        HICON hGear = load_png_icon("res\\settings_icon.png", 20);
        if (hGear) {
            SendMessageA(g_btn_quality_hwnd, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hGear);
        } else {
            SetWindowTextA(g_btn_quality_hwnd, "\xE2\x9A\x99");
        }

        g_status_hwnd = CreateWindowA("STATIC", "",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       402, 38, 400, 18, hwnd, NULL, g_hInst, NULL);

        g_canvas_hwnd = CreateWindowA(CANVAS_CLASS, "",
                                       WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_TABSTOP,
                                       0, TOOLBAR_H, 400, 400, hwnd, NULL, g_hInst, NULL);

        g_canvas_orig_wndproc = (WNDPROC)SetWindowLongPtrA(g_canvas_hwnd, GWLP_WNDPROC, (LONG_PTR)CanvasKeyProc);
        LOGI("Canvas subclassed, orig_wndproc=%p, err=%d", g_canvas_orig_wndproc, GetLastError());

        refresh_devices();
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_combo_hwnd, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) {
                    if (g_mirroring) {
                        auto dev = get_selected_device();
                        if (!dev.serial.empty() && dev.serial != g_hdc->serial()) {
                            g_config_initialized = false;
                            start_mirroring(dev.serial, dev.ip, dev.port);
                        }
                    } else {
                        EnableWindow(g_btn_start_hwnd, TRUE);
                    }
                }
            }
            break;
        case ID_BTN_START: {
            if (g_mirroring) {
                stop_mirroring();
            } else {
                auto dev = get_selected_device();
                if (!dev.serial.empty()) start_mirroring(dev.serial, dev.ip, dev.port);
            }
            break;
        }
        case ID_BTN_REFRESH:
            refresh_devices();
            break;
        case ID_BTN_BACK:
            if (g_input) g_input->inject_back();
            break;
        case ID_BTN_HOME:
            if (g_input) g_input->inject_home();
            break;
        case ID_BTN_RECENT:
            if (g_input) g_input->inject_recent();
            break;
        case ID_BTN_VOLUP:
            if (g_input) g_input->inject_key_event("16");
            break;
        case ID_BTN_VOLDOWN:
            if (g_input) g_input->inject_key_event("17");
            break;
        case ID_BTN_POWER:
            if (g_input) g_input->inject_power();
            break;
        case ID_BTN_REBOOT:
            if (g_mirroring) {
                if (center_msgbox("Reboot device?", "HScrcpy", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    if (g_input) g_input->inject_reboot();
                }
            }
            break;
        case ID_BTN_QUALITY:
            show_settings_dialog();
            break;
        }
        break;
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        MoveWindow(g_canvas_hwnd, 0, TOOLBAR_H, rc.right, rc.bottom - TOOLBAR_H, FALSE);
        destroy_backbuffer();
        InvalidateRect(g_canvas_hwnd, NULL, FALSE);
        break;
    }
    case WM_TIMER:
        if (wp == 1 && g_mirroring) {
            try_update_frame();
            if (!g_cjk_buf.empty() && g_cjk_buf_time > 0 && GetTickCount64() - g_cjk_buf_time >= 100) {
                flush_cjk_buffer();
            }
        }
        if (wp == 2) {
            KillTimer(hwnd, 2);
            std::string auto_serial = g_cli_args.serial;
            if (!auto_serial.empty()) {
                LOGI("Auto-start with serial: %s", auto_serial.c_str());
                int idx = (int)SendMessageA(g_combo_hwnd, CB_FINDSTRINGEXACT, -1, (LPARAM)auto_serial.c_str());
                if (idx != CB_ERR) {
                    SendMessageA(g_combo_hwnd, CB_SETCURSEL, idx, 0);
                    EnableWindow(g_btn_start_hwnd, TRUE);
                }
                std::string ip = "127.0.0.1", port = "8710";
                if (idx != CB_ERR && idx < (int)g_devices.size()) {
                    ip = g_devices[idx].ip;
                    port = g_devices[idx].port;
                } else {
                    auto colon_pos = auto_serial.find(':');
                    if (colon_pos != std::string::npos) {
                        ip = auto_serial.substr(0, colon_pos);
                        port = auto_serial.substr(colon_pos + 1);
                    }
                }
                start_mirroring(auto_serial, ip, port);
            } else {
                LOGW("No -s serial specified for auto-start");
            }
        }
        break;
    case WM_USER + 1: {
        g_starting = false;
        show_error("Failed to start screen capture.");
        auto *cap = g_capture;
        auto *hdc_ptr = g_hdc;
        g_capture = nullptr;
        g_hdc = nullptr;
        EnableWindow(g_btn_start_hwnd, TRUE);
        set_status("Failed");
        join_cleanup_thread();
        g_cleanup_thread = std::thread([cap, hdc_ptr]() {
            if (cap) { cap->stop(); delete cap; }
            delete hdc_ptr;
        });
        break;
    }
    case WM_USER + 2: {
        g_starting = false;
        show_error("No frame received.");
        auto *cap = g_capture;
        auto *hdc_ptr = g_hdc;
        g_capture = nullptr;
        g_hdc = nullptr;
        EnableWindow(g_btn_start_hwnd, TRUE);
        set_status("No frame");
        join_cleanup_thread();
        g_cleanup_thread = std::thread([cap, hdc_ptr]() {
            if (cap) { cap->stop(); delete cap; }
            delete hdc_ptr;
        });
        break;
    }
    case WM_USER + 3: {
        g_starting = false;
        g_screen_w = g_capture->width();
        g_screen_h = g_capture->height();

        g_last_frame_tick = GetTickCount();
        g_input = new InputInjector(*g_hdc, g_capture->local_base_port());
        join_startup_thread();
        g_startup_thread = std::thread([]() {
            if (g_input) g_input->init(g_capture ? g_capture->uitest_version() : "");
        });

        g_mirroring = true;
        SetWindowTextA(g_btn_start_hwnd, "Stop Mirroring");
        EnableWindow(g_btn_start_hwnd, TRUE);

        double aspect = (double)g_screen_w / g_screen_h;
        int min_w = 440;
        int init_h = 480;
        int init_w = (int)(init_h * aspect);
        if (init_w < min_w) {
            init_w = min_w;
            init_h = (int)(init_w / aspect);
        }

        RECT wr = {0, 0, init_w, init_h + TOOLBAR_H};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(g_hwnd, NULL, CW_USEDEFAULT, CW_USEDEFAULT,
                     wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);

        std::string serial = g_hdc->serial();
        char buf[256];
        const char *mode_str = g_capture->mode() == CaptureMode::kGrpcStream ? "gRPC" : "snapshot";
        snprintf(buf, sizeof(buf), "HScrcpy - %s | Device %ux%u | Stream %ux%u (%s, scale=%d, %s, %dfps)",
                 serial.c_str(), g_device_screen_w, g_device_screen_h, g_screen_w, g_screen_h, mode_str,
                 g_capture_config.scale, bitrate_label(g_capture_config.bit_rate),
                 g_capture_config.frame_rate);
        SetWindowTextA(g_hwnd, buf);
        set_status(buf);

        if (g_capture->mode() == CaptureMode::kSnapshotFallback && !g_snapshot_warned) {
            g_snapshot_warned = true;
            center_msgbox("Virtual screen streaming unavailable (FFmpeg DLLs not found or gRPC failed).\n"
                          "Falling back to screenshot mode (~2 FPS).\n\n"
                          "To enable gRPC virtual screen streaming, download FFmpeg shared DLLs:\n"
                          "https://github.com/BtbN/FFmpeg-Builds/releases\n"
                          "Copy avcodec-62.dll, avutil-60.dll, swscale-9.dll, swresample-6.dll\n"
                          "to the exe directory.",
                          "HScrcpy - Screenshot Mode", MB_OK | MB_ICONWARNING);
        }
        break;
    }
    case WM_USER + 4: {
        g_stopping = false;
        std::string cleaned = std::move(g_cleanup_serial);
        g_cleanup_serial.clear();

        if (!g_pending_start_serial.empty()) {
            std::string s = std::move(g_pending_start_serial);
            std::string i = std::move(g_pending_start_ip);
            std::string p = std::move(g_pending_start_port);
            g_pending_start_serial.clear();
            g_pending_start_ip.clear();
            g_pending_start_port.clear();
            start_mirroring(s, i, p);
        } else {
            if (!g_mirroring && !g_starting) {
                set_status("Stopped");
            }
        }
        break;
    }
    case WM_USER + 5: {
        if (!g_mirroring || !g_capture || !g_hdc) break;

        double aspect = (double)g_screen_w / g_screen_h;
        int min_w = 440;
        int init_h = 480;
        int init_w = (int)(init_h * aspect);
        if (init_w < min_w) {
            init_w = min_w;
            init_h = (int)(init_w / aspect);
        }

        RECT wr = {0, 0, init_w, init_h + TOOLBAR_H};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(g_hwnd, NULL, 0, 0,
                     wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER);

        std::string serial = g_hdc->serial();
        char buf[256];
        const char *mode_str = g_capture->mode() == CaptureMode::kGrpcStream ? "gRPC" : "snapshot";
        snprintf(buf, sizeof(buf), "HScrcpy - %s | Device %ux%u | Stream %ux%u (%s, scale=%d, %s, %dfps)",
                 serial.c_str(), g_device_screen_w, g_device_screen_h, g_screen_w, g_screen_h, mode_str,
                 g_capture_config.scale, bitrate_label(g_capture_config.bit_rate),
                 g_capture_config.frame_rate);
        SetWindowTextA(g_hwnd, buf);
        set_status(buf);
        break;
    }
    case WM_USER + 6: {
        char buf[256];
        snprintf(buf, sizeof(buf), "Connecting... (scale=%d, %s, %dfps)",
                 g_capture_config.scale, bitrate_label(g_capture_config.bit_rate),
                 g_capture_config.frame_rate);
        set_status(buf);
        break;
    }
    case WM_USER + 7: {
        if (g_mirroring && g_hdc) {
            std::string serial = g_hdc->serial();
            std::string ip = g_hdc->ip();
            std::string port = g_hdc->port();
            EnableWindow(g_btn_start_hwnd, FALSE);
            stop_mirroring();
            g_pending_start_serial = serial;
            g_pending_start_ip = ip;
            g_pending_start_port = port;
        }
        break;
    }
    case WM_DESTROY:
        destroy_backbuffer();
        join_cleanup_thread();
        join_startup_thread();
        if (g_canvas_orig_wndproc && g_canvas_hwnd) {
            SetWindowLongPtrA(g_canvas_hwnd, GWLP_WNDPROC, (LONG_PTR)g_canvas_orig_wndproc);
            g_canvas_orig_wndproc = nullptr;
        }
        if (g_mirroring || g_starting) {
            g_mirroring = false;
            g_starting = false;
            if (g_capture) { g_capture->stop(); delete g_capture; g_capture = nullptr; }
            if (g_input) { delete g_input; g_input = nullptr; }
            if (g_hdc) { delete g_hdc; g_hdc = nullptr; }
        }
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_hInst = hInst;

    Gdiplus::GdiplusStartupInput gdi_si;
    Gdiplus::GdiplusStartup(&g_gdiplus_token, &gdi_si, NULL);

    {
        std::string cmd_line(GetCommandLineA());
        std::vector<std::string> argv_strs;
        std::vector<char *> argv_vec;
        std::string token;
        bool in_quote = false;
        for (char c : cmd_line) {
            if (c == '"') { in_quote = !in_quote; }
            else if (c == ' ' && !in_quote) {
                if (!token.empty()) { argv_strs.push_back(token); token.clear(); }
            } else { token += c; }
        }
        if (!token.empty()) argv_strs.push_back(token);
        for (auto &s : argv_strs) argv_vec.push_back(s.data());
        int argc = static_cast<int>(argv_vec.size());
        if (!hscrcpy::CliParser::parse(g_cli_args, argc, argv_vec.data())) {
            hscrcpy::CliParser::print_usage("hscrcpy");
            return 1;
        }
        if (g_cli_args.show_help) {
            hscrcpy::CliParser::print_usage("hscrcpy");
            return 0;
        }
    }

    WNDCLASSEXA canvas_wc = {};
    canvas_wc.cbSize        = sizeof(canvas_wc);
    canvas_wc.style         = CS_DBLCLKS;
    canvas_wc.lpfnWndProc   = CanvasWndProc;
    canvas_wc.hInstance      = hInst;
    canvas_wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
    canvas_wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    canvas_wc.lpszClassName  = CANVAS_CLASS;
    RegisterClassExA(&canvas_wc);

    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInst;
    wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground  = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName  = WND_CLASS;
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(0, WND_CLASS, "HScrcpy",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 560, 720,
                              NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    SetTimer(g_hwnd, 1, 16, NULL);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (g_cli_args.auto_start) {
        SetTimer(g_hwnd, 2, 2000, NULL);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Gdiplus::GdiplusShutdown(g_gdiplus_token);
    return 0;
}
