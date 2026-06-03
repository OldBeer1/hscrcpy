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
static HWND g_hwnd = nullptr;
static HWND g_combo_hwnd = nullptr;
static HWND g_btn_start_hwnd = nullptr;
static HWND g_btn_refresh_hwnd = nullptr;
static HWND g_btn_power_hwnd = nullptr;
static HWND g_btn_back_hwnd = nullptr;
static HWND g_btn_volup_hwnd = nullptr;
static HWND g_btn_voldown_hwnd = nullptr;
static HWND g_btn_home_hwnd = nullptr;
static HWND g_btn_reboot_hwnd = nullptr;
static HWND g_status_hwnd = nullptr;
static HWND g_canvas_hwnd = nullptr;

static std::vector<HdcDevice> g_devices;

static DecodedImage g_cur_frame;
static std::mutex g_frame_mutex;
static uint64_t g_last_rendered_seq = 0;

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

static void show_error(const char *msg) {
    MessageBoxA(g_hwnd, msg, "HScrcpy Error", MB_OK | MB_ICONERROR);
}

static void set_status(const char *msg) {
    if (g_status_hwnd) SetWindowTextA(g_status_hwnd, msg);
}

static void refresh_devices() {
    SetWindowTextA(g_status_hwnd, "Scanning...");
    SendMessageA(g_combo_hwnd, CB_RESETCONTENT, 0, 0);

    HdcWrapper hdc_local;
    g_devices = hdc_local.list_devices();

    for (auto &d : g_devices) {
        std::string entry = d.serial;
        SendMessageA(g_combo_hwnd, CB_ADDSTRING, 0, (LPARAM)entry.c_str());
    }
    if (!g_devices.empty()) {
        SendMessageA(g_combo_hwnd, CB_SETCURSEL, 0, 0);
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
            if (!g_cur_frame.data.empty() && g_cur_frame.width > 0) {
                double aspect = (double)g_cur_frame.width / g_cur_frame.height;
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
                bih.biWidth    = static_cast<LONG>(g_cur_frame.width);
                bih.biHeight   = -static_cast<LONG>(g_cur_frame.height);
                bih.biPlanes   = 1;
                bih.biBitCount = 24;
                bih.biCompression = BI_RGB;

                SetStretchBltMode(memdc, HALFTONE);
                StretchDIBits(memdc, off_x, off_y, draw_w, draw_h,
                              0, 0, g_cur_frame.width, g_cur_frame.height,
                              g_cur_frame.data.data(),
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
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            auto [x, y] = canvas_to_screen(pt.x, pt.y);
            g_mouse_down = true;
            g_drag_start_x = x; g_drag_start_y = y;
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
            g_input->inject_touch_up(x, y);
            if (dx * dx + dy * dy >= 100) {
                g_input->inject_swipe(g_drag_start_x, g_drag_start_y, x, y);
            } else if (ms > 500) {
                g_input->inject_long_click(x, y);
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
    if (fc != g_last_drawn_frame) {
        DecodedImage img;
        if (g_capture->get_latest_frame(img) && !img.data.empty()) {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_cur_frame = std::move(img);
            g_last_drawn_frame = fc;
            InvalidateRect(g_canvas_hwnd, NULL, FALSE);
        }
    }
}

static void stop_mirroring() {
    if (!g_mirroring) return;
    g_mirroring = false;

    if (g_capture) { g_capture->stop(); delete g_capture; g_capture = nullptr; }
    if (g_input) { delete g_input; g_input = nullptr; }
    if (g_hdc) { delete g_hdc; g_hdc = nullptr; }

    g_screen_w = 0;
    g_screen_h = 0;
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        g_cur_frame = DecodedImage();
    }

    SetWindowTextA(g_btn_start_hwnd, "Start Mirroring");
    EnableWindow(g_combo_hwnd, TRUE);
    EnableWindow(g_btn_refresh_hwnd, TRUE);
    set_status("Stopped");
    g_snapshot_warned = false;

    InvalidateRect(g_canvas_hwnd, NULL, TRUE);
}

static void start_mirroring(const std::string &serial) {
    if (g_mirroring) {
        stop_mirroring();
        return;
    }

    set_status("Connecting...");
    EnableWindow(g_btn_start_hwnd, FALSE);

    g_hdc = new HdcWrapper(serial);
    g_capture = new ScreenCapture(*g_hdc, 30);

    if (!g_capture->start()) {
        show_error("Failed to start screen capture.");
        delete g_capture; g_capture = nullptr;
        delete g_hdc; g_hdc = nullptr;
        EnableWindow(g_btn_start_hwnd, TRUE);
        set_status("Failed");
        return;
    }

    set_status("Waiting for frame...");
    for (int i = 0; i < 300 && g_capture->width() == 0; i++) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Sleep(100);
    }

    if (g_capture->width() == 0) {
        show_error("No frame received.");
        g_capture->stop();
        delete g_capture; g_capture = nullptr;
        delete g_hdc; g_hdc = nullptr;
        EnableWindow(g_btn_start_hwnd, TRUE);
        set_status("No frame");
        return;
    }

    g_screen_w = g_capture->width();
    g_screen_h = g_capture->height();

    g_input = new InputInjector(*g_hdc);
    std::thread input_init_thread([]() {
        g_input->init(g_capture->uitest_version());
    });
    input_init_thread.detach();

    g_mirroring = true;
    SetWindowTextA(g_btn_start_hwnd, "Stop Mirroring");
    EnableWindow(g_btn_start_hwnd, TRUE);
    EnableWindow(g_combo_hwnd, FALSE);
    EnableWindow(g_btn_refresh_hwnd, FALSE);

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

        g_btn_power_hwnd = CreateWindowA("BUTTON", "Power",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          64, 34, 48, 22, hwnd, (HMENU)ID_BTN_POWER, g_hInst, NULL);

        g_btn_volup_hwnd = CreateWindowA("BUTTON", "Vol+",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          116, 34, 42, 22, hwnd, (HMENU)ID_BTN_VOLUP, g_hInst, NULL);

        g_btn_voldown_hwnd = CreateWindowA("BUTTON", "Vol-",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                            162, 34, 42, 22, hwnd, (HMENU)ID_BTN_VOLDOWN, g_hInst, NULL);

        g_btn_home_hwnd = CreateWindowA("BUTTON", "Home",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         208, 34, 46, 22, hwnd, (HMENU)ID_BTN_HOME, g_hInst, NULL);

        g_btn_reboot_hwnd = CreateWindowA("BUTTON", "Reboot",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                            258, 34, 50, 22, hwnd, (HMENU)ID_BTN_REBOOT, g_hInst, NULL);

        g_status_hwnd = CreateWindowA("STATIC", "",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       320, 38, 200, 18, hwnd, NULL, g_hInst, NULL);

        g_canvas_hwnd = CreateWindowA(CANVAS_CLASS, "",
                                       WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                       0, TOOLBAR_H, 400, 400, hwnd, NULL, g_hInst, NULL);

        refresh_devices();
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_combo_hwnd, CB_GETCURSEL, 0, 0);
                EnableWindow(g_btn_start_hwnd, sel != CB_ERR && !g_mirroring);
            }
            break;
        case ID_BTN_START: {
            if (g_mirroring) {
                stop_mirroring();
            } else {
                std::string serial = get_selected_serial();
                if (!serial.empty()) start_mirroring(serial);
            }
            break;
        }
        case ID_BTN_REFRESH:
            refresh_devices();
            break;
        case ID_BTN_POWER:
            if (g_input) g_input->inject_power();
            break;
        case ID_BTN_BACK:
            if (g_input) g_input->inject_back();
            break;
        case ID_BTN_VOLUP:
            if (g_input) g_input->inject_key_event("16");
            break;
        case ID_BTN_VOLDOWN:
            if (g_input) g_input->inject_key_event("17");
            break;
        case ID_BTN_HOME:
            if (g_input) g_input->inject_home();
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
    case WM_KEYDOWN:
        if (g_mirroring && g_input) {
            if (wp == VK_BACK) g_input->inject_back();
            if (wp == VK_HOME) g_input->inject_home();
        }
        break;
    case WM_TIMER:
        if (wp == 1 && g_mirroring) {
            try_update_frame();
        }
        if (wp == 2) {
            KillTimer(hwnd, 2);
            const char *auto_serial = "FMR0224B14009124";
            LOGI("Auto-start with serial=%s", auto_serial);
            int idx = (int)SendMessageA(g_combo_hwnd, CB_FINDSTRINGEXACT, -1, (LPARAM)auto_serial);
            if (idx != CB_ERR) {
                SendMessageA(g_combo_hwnd, CB_SETCURSEL, idx, 0);
                EnableWindow(g_btn_start_hwnd, TRUE);
            }
            start_mirroring(auto_serial);
        }
        break;
    case WM_DESTROY:
        if (g_mirroring) stop_mirroring();
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

    SetTimer(g_hwnd, 2, 2000, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
