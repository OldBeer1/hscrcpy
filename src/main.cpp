#include "cli.h"
#include "hdc_wrapper.h"
#include "screen_capture.h"
#include "input_injector.h"
#include "h264_decoder.h"
#include "log.h"
#include "version.h"
#include <cstdio>
#include <memory>
#include <chrono>
#include <algorithm>

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

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

static void show_error(const char *msg) {
    MessageBoxA(g_hwnd, msg, "HScrcpy Error", MB_OK | MB_ICONERROR);
}

static void set_status(const char *msg) {
    if (g_status_hwnd) SetWindowTextA(g_status_hwnd, msg);
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

    return {static_cast<uint32_t>(rel_x * g_screen_w / draw_w),
            static_cast<uint32_t>(rel_y * g_screen_h / draw_h)};
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

        HDC memdc = CreateCompatibleDC(hdc);
        HBITMAP membmp = CreateCompatibleBitmap(hdc, cw, ch);
        HBITMAP oldbmp = (HBITMAP)SelectObject(memdc, membmp);

        HBRUSH bg = CreateSolidBrush(RGB(30, 30, 30));
        FillRect(memdc, &rc, bg);
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

                SetStretchBltMode(memdc, HALFTONE);
                StretchDIBits(memdc, off_x, off_y, draw_w, draw_h,
                              0, 0, g_cur_frame->width, g_cur_frame->height,
                              g_cur_frame->data.data(),
                              reinterpret_cast<BITMAPINFO *>(&bih),
                              DIB_RGB_COLORS, SRCCOPY);
            }
        }

        BitBlt(hdc, 0, 0, cw, ch, memdc, 0, 0, SRCCOPY);
        SelectObject(memdc, oldbmp);
        DeleteObject(membmp);
        DeleteDC(memdc);

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
            g_input->inject_touch_down(x, y);
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_mouse_down && g_mirroring && g_input) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            auto [x, y] = canvas_to_screen(pt.x, pt.y);
            g_input->inject_touch_move(x, y);
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
                g_input->inject_touch_up(x, y);
            } else if (ms > 500) {
                g_input->inject_touch_up(x, y);
                g_input->inject_long_click(x, y);
            } else {
                g_input->inject_touch_up(x, y);
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

static void try_update_frame() {
    if (!g_capture) return;
    auto fc = g_capture->frame_count();
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
        }
    }
}

static bool g_stopping = false;
static std::string g_pending_start_serial;
static std::string g_pending_start_ip;
static std::string g_pending_start_port;

static void stop_mirroring() {
    if (!g_mirroring && !g_starting) return;
    if (g_stopping) return;

    g_mirroring = false;
    g_starting = false;
    g_stopping = true;

    g_screen_w = 0;
    g_screen_h = 0;
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        g_cur_frame.reset();
    }

    SetWindowTextA(g_btn_start_hwnd, "Start Mirroring");
    EnableWindow(g_btn_start_hwnd, FALSE);
    set_status("Stopping...");
    g_snapshot_warned = false;

    InvalidateRect(g_canvas_hwnd, NULL, TRUE);

    auto *cap = g_capture;
    auto *inp = g_input;
    auto *hdc = g_hdc;
    g_capture = nullptr;
    g_input = nullptr;
    g_hdc = nullptr;

    std::thread([cap, inp, hdc]() {
        if (cap) { cap->stop(); delete cap; }
        if (inp) { delete inp; }
        if (hdc) { delete hdc; }
        PostMessageA(g_hwnd, WM_USER + 4, 0, 0);
    }).detach();
}

static void start_mirroring(const std::string &serial, const std::string &ip = "127.0.0.1", const std::string &port = "8710") {
    if (g_mirroring || g_starting) {
        return;
    }
    if (g_stopping) {
        g_pending_start_serial = serial;
        g_pending_start_ip = ip;
        g_pending_start_port = port;
        return;
    }

    g_starting = true;
    set_status("Connecting...");
    EnableWindow(g_btn_start_hwnd, FALSE);

    g_hdc = new HdcWrapper(serial, ip, port);
    g_capture = new ScreenCapture(*g_hdc, 30);

    std::thread([serial, ip, port]() {
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
    }).detach();
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
                                         12, 34, 48, 22, hwnd, (HMENU)ID_BTN_BACK, g_hInst, NULL);
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

        g_status_hwnd = CreateWindowA("STATIC", "",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       370, 38, 400, 18, hwnd, NULL, g_hInst, NULL);

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
                if (MessageBoxA(g_hwnd, "Reboot device?", "HScrcpy", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    if (g_input) g_input->inject_reboot();
                }
            }
            break;
        }
        break;
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        MoveWindow(g_canvas_hwnd, 0, TOOLBAR_H, rc.right, rc.bottom - TOOLBAR_H, FALSE);
        InvalidateRect(g_canvas_hwnd, NULL, FALSE);
        break;
    }
    case WM_TIMER:
        if (wp == 1 && g_mirroring) {
            try_update_frame();
            if (!g_cjk_buf.empty() && g_cjk_buf_time > 0 && GetTickCount64() - g_cjk_buf_time >= 100) {
                flush_cjk_buffer();
            }
            if (g_capture && g_capture->frame_event()) {
                WaitForSingleObject(g_capture->frame_event(), 33);
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
        std::thread([cap, hdc_ptr]() {
            if (cap) { cap->stop(); delete cap; }
            delete hdc_ptr;
        }).detach();
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
        std::thread([cap, hdc_ptr]() {
            if (cap) { cap->stop(); delete cap; }
            delete hdc_ptr;
        }).detach();
        break;
    }
    case WM_USER + 3: {
        g_starting = false;
        g_screen_w = g_capture->width();
        g_screen_h = g_capture->height();

        g_input = new InputInjector(*g_hdc, g_capture->local_base_port());
        std::thread input_init_thread([]() {
            g_input->init(g_capture->uitest_version());
        });
        input_init_thread.detach();

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
        snprintf(buf, sizeof(buf), "HScrcpy - %s %ux%u (%s)",
                 serial.c_str(), g_screen_w, g_screen_h, mode_str);
        SetWindowTextA(g_hwnd, buf);
        set_status(buf);

        if (g_capture->mode() == CaptureMode::kSnapshotFallback && !g_snapshot_warned) {
            g_snapshot_warned = true;
            MessageBoxA(g_hwnd,
                        "Virtual screen streaming unavailable (FFmpeg DLLs not found or gRPC failed).\n"
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
        EnableWindow(g_btn_start_hwnd, TRUE);
        set_status("Stopped");
        if (!g_pending_start_serial.empty()) {
            std::string s = std::move(g_pending_start_serial);
            std::string i = std::move(g_pending_start_ip);
            std::string p = std::move(g_pending_start_port);
            g_pending_start_serial.clear();
            g_pending_start_ip.clear();
            g_pending_start_port.clear();
            start_mirroring(s, i, p);
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
        snprintf(buf, sizeof(buf), "HScrcpy - %s %ux%u (%s)",
                 serial.c_str(), g_screen_w, g_screen_h, mode_str);
        SetWindowTextA(g_hwnd, buf);
        set_status(buf);
        break;
    }
    case WM_DESTROY:
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

    SetTimer(g_hwnd, 1, 33, NULL);

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

    return 0;
}
