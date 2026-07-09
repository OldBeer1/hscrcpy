#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <objidl.h>
#include <gdiplus.h>
#include <d2d1.h>
#pragma comment(lib, "gdiplus.lib")

#include "cli.h"
#include "config.h"
#include "screen_capture.h"
#include "hdc_wrapper.h"
#include "input_injector.h"
#include "h264_decoder.h"
#include "layout_window.h"
#include "file_manager_window.h"
#include "cph_window.h"
#include "log.h"
#include "version.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <algorithm>
#include <atomic>
#include <thread>
#include <ctime>
#include <sstream>
#include <string>

using namespace hscrcpy;

static const char *WND_CLASS = "HScrcpyWndClass";
static const char *CANVAS_CLASS = "HScrcpyCanvasClass";

static HINSTANCE g_hInst = nullptr;
static HdcWrapper *g_hdc = nullptr;
static ScreenCapture *g_capture = nullptr;
static InputInjector *g_input = nullptr;
static uint32_t g_screen_w = 0, g_screen_h = 0;
static std::string g_sized_serial;  // serial of the phone last auto-sized (per-phone first-connect)
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
static HWND g_btn_layout_hwnd = nullptr;
static HWND g_btn_fm_hwnd = nullptr;
static HWND g_btn_cph_hwnd = nullptr;
static HWND g_btn_record_hwnd = nullptr;
static HWND g_btn_screenshot_hwnd = nullptr;
static HWND g_status_hwnd = nullptr;
static HWND g_canvas_hwnd = nullptr;
static bool g_layout_visible = false;
static bool g_fm_visible = false;
static bool g_cph_visible = false;
static bool g_is_cloud_phone = false;

// Recording state (phone-side aa start screenrecorder + mediatool pull)
static std::atomic<bool> g_recording{false};
static std::string g_record_name;          // CustomizedFileName, e.g. hscrcpy_<ts>.mp4
static std::string g_record_local_path;    // downloaded recording path (for toast+open on UI thread)
static std::string g_screenshot_local_path; // downloaded screenshot path (for toast+open on UI thread)
static std::string g_record_serial;        // serial captured at start (decouples from g_hdc lifetime)
static std::thread g_record_thread;
static constexpr int kRecordTimerId = 4;    // 30-min auto-stop timer
static constexpr UINT kRecordMaxMs = 30 * 60 * 1000;

// Recording/screenshot UI notification messages (WM_USER + 11..13, after WmNotify)
static constexpr UINT kMsgRecordStarted  = WM_USER + 11;
static constexpr UINT kMsgRecordStopped  = WM_USER + 12;
static constexpr UINT kMsgScreenshotDone = WM_USER + 13;

// Splitter bar child window
static HWND g_splitter_hwnd = nullptr;
static bool g_splitter_dragging = false;
static int g_splitter_drag_offset = 0;

static LRESULT CALLBACK SplitterWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static std::vector<HdcDevice> g_devices;
static hscrcpy::CliArgs g_cli_args;

bool g_verbose = false;

static std::shared_ptr<DecodedImage> g_cur_frame;
static std::mutex g_frame_mutex;
static uint64_t g_last_rendered_seq = 0;
static HANDLE g_frame_event = nullptr;

static int TOOLBAR_H = 68;
static bool g_snapshot_warned = false;

static bool g_mouse_down = false;

static CaptureConfig g_capture_config;
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
#define ID_BTN_LAYOUT  1012
#define ID_BTN_FM      1013
#define ID_BTN_CPH     1014
#define ID_BTN_RECORD  1015
#define ID_BTN_SCREENSHOT 1016

static ULONG_PTR g_gdiplus_token = 0;

// Direct2D rendering state
static ID2D1Factory *g_d2d_factory = nullptr;
static ID2D1HwndRenderTarget *g_d2d_rt = nullptr;
static ID2D1Bitmap *g_d2d_bitmap = nullptr;
static bool g_d2d_available = false;
static bool g_d2d_bitmap_current = false;  // true when bitmap matches latest g_cur_frame
static uint32_t g_d2d_bmp_w = 0, g_d2d_bmp_h = 0;

// Cached D2D brushes for layout highlight overlay
static ID2D1SolidColorBrush *g_d2d_hl_fill = nullptr;
static ID2D1SolidColorBrush *g_d2d_hl_border = nullptr;
// Cached D2D brush for the top separator line (canvas/toolbar boundary)
static ID2D1SolidColorBrush *g_d2d_sep_brush = nullptr;

static bool init_d2d() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d_factory);
    if (FAILED(hr)) {
        LOGW("D2D1CreateFactory failed (hr=0x%08X), using GDI fallback", (unsigned)hr);
        return false;
    }
    g_d2d_available = true;
    LOGI("Direct2D initialized");
    return true;
}

static void cleanup_d2d() {
    if (g_d2d_hl_fill) { g_d2d_hl_fill->Release(); g_d2d_hl_fill = nullptr; }
    if (g_d2d_hl_border) { g_d2d_hl_border->Release(); g_d2d_hl_border = nullptr; }
    if (g_d2d_sep_brush) { g_d2d_sep_brush->Release(); g_d2d_sep_brush = nullptr; }
    if (g_d2d_bitmap) { g_d2d_bitmap->Release(); g_d2d_bitmap = nullptr; }
    if (g_d2d_rt) { g_d2d_rt->Release(); g_d2d_rt = nullptr; }
    if (g_d2d_factory) { g_d2d_factory->Release(); g_d2d_factory = nullptr; }
    g_d2d_available = false;
    g_d2d_bitmap_current = false;
    g_d2d_bmp_w = 0;
    g_d2d_bmp_h = 0;
}

static bool ensure_d2d_render_target() {
    if (!g_d2d_factory || !g_canvas_hwnd) return false;
    RECT rc;
    GetClientRect(g_canvas_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right, rc.bottom);
    if (!g_d2d_rt) {
        HRESULT hr = g_d2d_factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(g_canvas_hwnd, size),
            &g_d2d_rt);
        if (FAILED(hr)) {
            LOGW("CreateHwndRenderTarget failed (hr=0x%08X)", (unsigned)hr);
            cleanup_d2d();
            return false;
        }
        g_d2d_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    } else {
        g_d2d_rt->Resize(size);
    }
    return true;
}

// Resize main window to match current frame aspect ratio (no black bars).
// Formula: init_h=480, init_w=480×aspect, if init_w<360 then init_w=360 and
// recalc init_h to preserve aspect. Skipped when maximized (SetWindowPos is
// ignored on maximized windows anyway — see feedback_maximized_setwindowpos.md).
static void resize_window_to_frame_aspect(HWND hwnd) {
    if (g_screen_w <= 0 || g_screen_h <= 0 || IsZoomed(hwnd)) return;
    double aspect = (double)g_screen_w / g_screen_h;
    int init_h = 480;
    int init_w = (int)(init_h * aspect);
    if (init_w < 360) {
        init_w = 360;
        init_h = (int)(init_w / aspect);
    }
    // When a sub-window panel is visible, add its width so the canvas keeps
    // its correct aspect-ratio width instead of being squished by the panel.
    int panel_w = 0;
    if (g_layout_visible) panel_w = LayoutWindow::panel_width();
    else if (g_fm_visible) panel_w = FileManagerWindow::panel_width();
    else if (g_cph_visible) panel_w = CphWindow::panel_width();
    RECT wr = {0, 0, init_w + panel_w, init_h + TOOLBAR_H};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(hwnd, NULL, 0, 0,
                 wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

static void upload_frame_to_d2d(const DecodedImage &img) {
    if (!g_d2d_rt) return;

    // Bitmap width = linesize (img.stride/4), NOT display width (img.width).
    // sws writes rows of `stride` bytes = linesize*4. Bitmap must be linesize-wide
    // so stride == bitmap_width*4 (no D2D misalignment). Display crop via srcRect at render.
    UINT bmp_w = static_cast<UINT>(img.stride / 4);
    UINT bmp_h = img.height;

    // Recreate bitmap if size changed
    if (!g_d2d_bitmap || g_d2d_bmp_w != bmp_w || g_d2d_bmp_h != bmp_h) {
        if (g_d2d_bitmap) { g_d2d_bitmap->Release(); g_d2d_bitmap = nullptr; }
        D2D1_SIZE_U sz = D2D1::SizeU(bmp_w, bmp_h);
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        HRESULT hr = g_d2d_rt->CreateBitmap(sz, props, &g_d2d_bitmap);
        if (FAILED(hr)) {
            LOGW("CreateBitmap failed (hr=0x%08X)", (unsigned)hr);
            return;
        }
        g_d2d_bmp_w = bmp_w;
        g_d2d_bmp_h = bmp_h;
        LOGI("D2D bitmap created: %ux%u (display=%ux%u) stride=%d data_size=%zu",
             bmp_w, bmp_h, img.width, img.height, img.stride, img.data.size());
    }

    D2D1_RECT_U rc = D2D1::RectU(0, 0, bmp_w, bmp_h);
    HRESULT hr = g_d2d_bitmap->CopyFromMemory(&rc, img.data.data(), img.stride);
    if (FAILED(hr)) {
        LOGW("CopyFromMemory failed (hr=0x%08X)", (unsigned)hr);
    }
}

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

static bool ensure_backbuffer(HDC ref_dc, int w, int h) {
    if (g_backbuffer_dc && g_backbuffer_w >= w && g_backbuffer_h >= h) return true;
    destroy_backbuffer();
    g_backbuffer_dc = CreateCompatibleDC(ref_dc);
    g_backbuffer_bmp = CreateCompatibleBitmap(ref_dc, w, h);
    if (!g_backbuffer_dc || !g_backbuffer_bmp) {
        destroy_backbuffer();
        return false;
    }
    g_backbuffer_old = (HBITMAP)SelectObject(g_backbuffer_dc, g_backbuffer_bmp);
    g_backbuffer_w = w;
    g_backbuffer_h = h;
    return true;
}

static HICON load_png_icon(const char *relative_path, int size,
                            BYTE tint_r = 0, BYTE tint_g = 0, BYTE tint_b = 0,
                            bool tint = false) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string dir(exe_path);
    auto sep = dir.find_last_of("\\/");
    if (sep != std::string::npos) dir = dir.substr(0, sep + 1);
    std::string path = dir + relative_path;

    HICON icon = NULL;
    Gdiplus::Bitmap *src = Gdiplus::Bitmap::FromFile(std::wstring(path.begin(), path.end()).c_str());
    if (!src || src->GetLastStatus() != Gdiplus::Ok) { delete src; return NULL; }

    int sw = src->GetWidth(), sh = src->GetHeight();

    if (tint) {
        // Scale source first
        Gdiplus::Bitmap *scaled = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
        { Gdiplus::Graphics gs(scaled);
          gs.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
          gs.Clear(Gdiplus::Color(0, 0, 0, 0));
          gs.DrawImage(src, 0, 0, size, size); }
        delete src; src = scaled;

        // Read alpha from scaled, write tinted premultiplied pixels to dst
        Gdiplus::BitmapData src_bd = {}, dst_bd = {};
        Gdiplus::Rect r(0, 0, size, size);
        src->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &src_bd);

        Gdiplus::Bitmap *dst = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
        dst->LockBits(&r, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &dst_bd);

        for (int y = 0; y < size; y++) {
            const BYTE *sp = (const BYTE *)src_bd.Scan0 + y * src_bd.Stride;
            BYTE *dp = (BYTE *)dst_bd.Scan0 + y * dst_bd.Stride;
            for (int x = 0; x < size; x++) {
                BYTE a = sp[3];
                dp[0] = (BYTE)(tint_b * a / 255);
                dp[1] = (BYTE)(tint_g * a / 255);
                dp[2] = (BYTE)(tint_r * a / 255);
                dp[3] = a;
                sp += 4; dp += 4;
            }
        }
        dst->UnlockBits(&dst_bd);
        src->UnlockBits(&src_bd);
        delete src; src = nullptr;

        dst->GetHICON(&icon);
        delete dst;
    } else {
        int dw = size, dh = size;
        if (sw * dh > sh * dw) dh = (int)(size * (double)sh / sw);
        else dw = (int)(size * (double)sw / sh);
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
static HWND g_msgbox_parent = NULL;

static LRESULT CALLBACK MsgBoxCBTProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HCBT_ACTIVATE) {
        HWND hMsg = (HWND)wp;
        if (g_msgbox_parent) {
            RECT pr, mr;
            GetWindowRect(g_msgbox_parent, &pr);
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

static int center_msgbox(const char *text, const char *title, UINT type, HWND hParent = NULL) {
    g_msgbox_parent = hParent ? hParent : g_hwnd;
    g_msgbox_hook = SetWindowsHookExA(WH_CBT, MsgBoxCBTProc, NULL, GetCurrentThreadId());
    int ret = MessageBoxA(g_msgbox_parent, text, title, type);
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
    if (bitrate >= 1048576) {
        static char buf[32];
        snprintf(buf, sizeof(buf), "%dMibps", bitrate / 1048576);
        return buf;
    }
    if (bitrate >= 1024) {
        static char buf[32];
        snprintf(buf, sizeof(buf), "%dKibps", bitrate / 1024);
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
    auto &cfg = AppConfig::instance();

    uint32_t eff_w = g_device_screen_w > 0 ? g_device_screen_w : 1260;

    if (is_tcp) {
        g_capture_config.scale = (eff_w > 720) ? cfg.tcp_scale : 1;
        g_capture_config.bit_rate = cfg.tcp_bitrate;
        g_capture_config.frame_rate = cfg.tcp_frame_rate;
        g_capture_config.iframe_interval = cfg.iframe_interval;
        LOGI("Auto TCP: %ux%u -> scale=%d, %s, %dfps", eff_w, g_device_screen_h,
             g_capture_config.scale, bitrate_label(g_capture_config.bit_rate), g_capture_config.frame_rate);
    } else {
        g_capture_config.scale = (eff_w > 720) ? cfg.usb_scale : 1;
        g_capture_config.bit_rate = cfg.usb_bitrate;
        g_capture_config.frame_rate = cfg.usb_frame_rate;
        g_capture_config.iframe_interval = cfg.iframe_interval;
        LOGI("Auto USB: %ux%u -> scale=%d, %s, %dfps", eff_w, g_device_screen_h,
             g_capture_config.scale, bitrate_label(g_capture_config.bit_rate), g_capture_config.frame_rate);
    }
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    static AppConfig *pending = nullptr;
    switch (msg) {
    case WM_INITDIALOG: {
        RECT pr, dr;
        GetWindowRect(g_hwnd, &pr);
        GetWindowRect(hDlg, &dr);
        int x = pr.left + (pr.right - pr.left - (dr.right - dr.left)) / 2;
        int y = pr.top + (pr.bottom - pr.top - (dr.bottom - dr.top)) / 2;
        SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        pending = new AppConfig(AppConfig::instance());

        auto init_combo = [&](int id, const auto &entries, int cur_val) {
            HWND h = GetDlgItem(hDlg, id);
            int sel = 0;
            for (int i = 0; i < (int)entries.size(); i++) {
                int idx = (int)SendMessageA(h, CB_ADDSTRING, 0, (LPARAM)entries[i].first);
                SendMessageA(h, CB_SETITEMDATA, idx, entries[i].second);
                if (entries[i].second == cur_val) sel = i;
            }
            SendMessageA(h, CB_SETCURSEL, sel, 0);
        };

        struct SE { const char *first; int second; };
        SE scale_entries[] = {{"1",1},{"2",2},{"3",3},{"4",4},{"5",5}};
        SE bitrate_entries[] = {
            {"4 Mibps",4194304},{"8 Mibps",8388608},
            {"12 Mibps",12582912},{"16 Mibps",16777216},
            {"20 Mibps",20971520},{"24 Mibps",25165824},
            {"30 Mibps",31457280}
        };
        SE fps_entries[] = {
            {"10 fps",10},{"15 fps",15},{"24 fps",24},{"30 fps",30},{"60 fps",60}
        };
        SE timeout_entries[] = {
            {"60s",60},{"120s",120},{"180s",180},{"300s",300},{"600s",600}
        };
        SE iframe_entries[] = {
            {"500ms",500},{"1000ms",1000},{"2000ms",2000},{"3000ms",3000},{"5000ms",5000}
        };

        std::vector<SE> sc(scale_entries, scale_entries + 5);
        std::vector<SE> br(bitrate_entries, bitrate_entries + 7);
        std::vector<SE> fp(fps_entries, fps_entries + 5);
        std::vector<SE> to(timeout_entries, timeout_entries + 5);
        std::vector<SE> ifr(iframe_entries, iframe_entries + 5);

        init_combo(110, sc, pending->usb_scale);
        init_combo(111, br, pending->usb_bitrate);
        init_combo(112, fp, pending->usb_frame_rate);

        init_combo(120, sc, pending->tcp_scale);
        init_combo(121, br, pending->tcp_bitrate);
        init_combo(122, fp, pending->tcp_frame_rate);

        init_combo(130, to, pending->idle_timeout_sec);
        init_combo(131, ifr, pending->iframe_interval);

        char port_buf[64];
        snprintf(port_buf, sizeof(port_buf), "%d", pending->local_base_port);
        SetWindowTextA(GetDlgItem(hDlg, 132), port_buf);
        snprintf(port_buf, sizeof(port_buf), "%d", pending->local_port_range);
        SetWindowTextA(GetDlgItem(hDlg, 133), port_buf);

        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            auto read_combo = [&](int id) -> int {
                HWND h = GetDlgItem(hDlg, id);
                int s = (int)SendMessageA(h, CB_GETCURSEL, 0, 0);
                return (int)SendMessageA(h, CB_GETITEMDATA, s, 0);
            };

            pending->usb_scale = read_combo(110);
            pending->usb_bitrate = read_combo(111);
            pending->usb_frame_rate = read_combo(112);

            pending->tcp_scale = read_combo(120);
            pending->tcp_bitrate = read_combo(121);
            pending->tcp_frame_rate = read_combo(122);

            pending->idle_timeout_sec = read_combo(130);
            pending->iframe_interval = read_combo(131);

            char buf[32];
            GetWindowTextA(GetDlgItem(hDlg, 132), buf, sizeof(buf));
            pending->local_base_port = atoi(buf);
            if (pending->local_base_port < 1024 || pending->local_base_port > 60000) {
                center_msgbox("Port Base must be between 1024 and 60000.", "Invalid Input", MB_OK | MB_ICONWARNING, hDlg);
                SetFocus(GetDlgItem(hDlg, 132));
                SendDlgItemMessageA(hDlg, 132, EM_SETSEL, 0, -1);
                return TRUE;
            }
            GetWindowTextA(GetDlgItem(hDlg, 133), buf, sizeof(buf));
            pending->local_port_range = atoi(buf);
            int max_range = 65535 - pending->local_base_port;
            if (pending->local_port_range < 10 || pending->local_port_range > max_range) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Port Range must be between 10 and %d.", max_range);
                center_msgbox(msg, "Invalid Input", MB_OK | MB_ICONWARNING, hDlg);
                SetFocus(GetDlgItem(hDlg, 133));
                SendDlgItemMessageA(hDlg, 133, EM_SETSEL, 0, -1);
                return TRUE;
            }

            AppConfig::instance() = *pending;
            config_save();

            CaptureConfig old_config = g_capture_config;
            bool is_tcp = g_hdc && g_hdc->is_tcp();
            g_capture_config.scale = is_tcp ? pending->tcp_scale : pending->usb_scale;
            g_capture_config.bit_rate = is_tcp ? pending->tcp_bitrate : pending->usb_bitrate;
            g_capture_config.frame_rate = is_tcp ? pending->tcp_frame_rate : pending->usb_frame_rate;
            g_capture_config.iframe_interval = pending->iframe_interval;

            LOGI("Config updated: usb=%d/%s/%dfps tcp=%d/%s/%dfps idle=%ds iframe=%dms port=%d+%d",
                 pending->usb_scale, bitrate_label(pending->usb_bitrate), pending->usb_frame_rate,
                 pending->tcp_scale, bitrate_label(pending->tcp_bitrate), pending->tcp_frame_rate,
                 pending->idle_timeout_sec, pending->iframe_interval,
                 pending->local_base_port, pending->local_port_range);

            delete pending;
            pending = nullptr;
            EndDialog(hDlg, IDOK);

            if (g_mirroring && (old_config.scale != g_capture_config.scale ||
                                old_config.bit_rate != g_capture_config.bit_rate ||
                                old_config.frame_rate != g_capture_config.frame_rate ||
                                old_config.iframe_interval != g_capture_config.iframe_interval)) {
                if (center_msgbox("Settings changed. Restart mirroring to apply?",
                                  "HScrcpy", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    PostMessageA(g_hwnd, kMsgConnLostStartup, 0, 0);
                }
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            delete pending;
            pending = nullptr;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        delete pending;
        pending = nullptr;
        break;
    }
    return FALSE;
}

static void show_settings_dialog() {
    struct {
        DLGTEMPLATE dlg;
        WORD menu, cls, title;
        WORD pt;
        WORD weight;
        BYTE italic, charset;
        wchar_t typeface[32];
    } tmpl = {};
    tmpl.dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_MODALFRAME | DS_SETFONT;
    tmpl.dlg.cx = 204;
    tmpl.dlg.cy = 144;
    tmpl.dlg.cdit = 25;
    tmpl.menu = 0;
    tmpl.cls = 0;
    tmpl.title = 0;
    tmpl.pt = 6;
    tmpl.weight = FW_NORMAL;
    tmpl.italic = 0;
    tmpl.charset = DEFAULT_CHARSET;
    wcscpy(tmpl.typeface, L"MS Shell Dlg");

    static std::vector<BYTE> data;
    data.resize(8192);
    BYTE *p = data.data();
    auto align = [&]() { p += (4 - ((ULONG_PTR)p % 4)) % 4; };
    auto write = [&](const void *src, size_t len) { memcpy(p, src, len); p += len; };

    write(&tmpl.dlg, sizeof(tmpl.dlg));
    p += sizeof(WORD) * 3;
    write(&tmpl.pt, sizeof(WORD));
    write(&tmpl.weight, sizeof(WORD));
    write(&tmpl.italic, 1);
    write(&tmpl.charset, 1);
    write(tmpl.typeface, (wcslen(tmpl.typeface) + 1) * sizeof(wchar_t));

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

    auto L = WS_CHILD | WS_VISIBLE | SS_LEFT;
    auto CB = WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    auto ED = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | WS_TABSTOP;
    auto GRP = WS_CHILD | WS_VISIBLE | BS_GROUPBOX;

    // USB Stream group (y=2, cy=32) — label cy=8 fixes text clipping; cx widened for colon
    writeItem(GRP, 4, 2, 198, 32, (WORD)-1, L"BUTTON", L"USB Stream");
    writeItem(L, 6, 15, 22, 8, (WORD)-1, L"STATIC", L"Scale:");
    writeItem(CB, 30, 13, 28, 200, 110, L"COMBOBOX", L"");
    writeItem(L, 62, 15, 30, 8, (WORD)-1, L"STATIC", L"Bitrate:");
    writeItem(CB, 94, 13, 46, 200, 111, L"COMBOBOX", L"");
    writeItem(L, 142, 15, 18, 8, (WORD)-1, L"STATIC", L"FPS:");
    writeItem(CB, 160, 13, 36, 200, 112, L"COMBOBOX", L"");

    // TCP Stream group (y=36, cy=32)
    writeItem(GRP, 4, 36, 198, 32, (WORD)-1, L"BUTTON", L"TCP Stream");
    writeItem(L, 6, 49, 22, 8, (WORD)-1, L"STATIC", L"Scale:");
    writeItem(CB, 30, 47, 28, 200, 120, L"COMBOBOX", L"");
    writeItem(L, 62, 49, 30, 8, (WORD)-1, L"STATIC", L"Bitrate:");
    writeItem(CB, 94, 47, 46, 200, 121, L"COMBOBOX", L"");
    writeItem(L, 142, 49, 18, 8, (WORD)-1, L"STATIC", L"FPS:");
    writeItem(CB, 160, 47, 36, 200, 122, L"COMBOBOX", L"");

    // Advanced group (y=70, cy=50) — col1 control x=50, col2 control x=120 (aligned)
    writeItem(GRP, 4, 70, 198, 50, (WORD)-1, L"BUTTON", L"Advanced");
    writeItem(L, 6, 83, 42, 8, (WORD)-1, L"STATIC", L"Idle Timeout:");
    writeItem(CB, 50, 81, 42, 200, 130, L"COMBOBOX", L"");
    writeItem(L, 96, 83, 22, 8, (WORD)-1, L"STATIC", L"IDR:");
    writeItem(CB, 120, 81, 40, 200, 131, L"COMBOBOX", L"");
    writeItem(L, 6, 99, 42, 8, (WORD)-1, L"STATIC", L"Port Base:");
    writeItem(ED, 50, 97, 38, 10, 132, L"EDIT", L"");
    writeItem(L, 96, 99, 48, 8, (WORD)-1, L"STATIC", L"Port Range:");
    writeItem(ED, 146, 97, 38, 10, 133, L"EDIT", L"");

    // Buttons (y=126)
    writeItem(WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 110, 126, 42, 11, IDOK, L"BUTTON", L"OK");
    writeItem(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 158, 126, 42, 11, IDCANCEL, L"BUTTON", L"Cancel");

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

// Shared layout calculation: compute the drawn image rect within the canvas
struct CanvasLayout {
    int draw_w, draw_h, off_x, off_y;
};

static CanvasLayout calc_canvas_layout() {
    CanvasLayout lay = {0, 0, 0, 0};
    if (g_screen_w == 0 || g_screen_h == 0) return lay;
    RECT rc;
    GetClientRect(g_canvas_hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return lay;

    double aspect = (double)g_screen_w / g_screen_h;
    if ((double)cw / ch > aspect) {
        lay.draw_h = ch;
        lay.draw_w = (int)(ch * aspect);
    } else {
        lay.draw_w = cw;
        lay.draw_h = (int)(cw / aspect);
    }
    if (lay.draw_w == cw - 1) lay.draw_w = cw;
    if (lay.draw_h == ch - 1) lay.draw_h = ch;
    lay.off_x = (cw - lay.draw_w) / 2;
    lay.off_y = (ch - lay.draw_h) / 2;
    return lay;
}

// Check if canvas coordinates (wx, wy) are within the rendered image area
static bool is_inside_canvas_image(int wx, int wy) {
    auto lay = calc_canvas_layout();
    if (lay.draw_w == 0) return false;
    return wx >= lay.off_x && wx <= lay.off_x + lay.draw_w &&
           wy >= lay.off_y && wy <= lay.off_y + lay.draw_h;
}

static std::pair<uint32_t, uint32_t> canvas_to_screen(int wx, int wy) {
    auto lay = calc_canvas_layout();
    if (lay.draw_w == 0) return {0, 0};

    int rel_x = wx - lay.off_x;
    int rel_y = wy - lay.off_y;
    if (rel_x < 0) rel_x = 0;
    if (rel_y < 0) rel_y = 0;
    if (rel_x > lay.draw_w) rel_x = lay.draw_w;
    if (rel_y > lay.draw_h) rel_y = lay.draw_h;

    uint32_t phy_w = g_screen_w * g_capture_config.scale;
    uint32_t phy_h = g_screen_h * g_capture_config.scale;

    return {static_cast<uint32_t>(rel_x * phy_w / lay.draw_w),
            static_cast<uint32_t>(rel_y * phy_h / lay.draw_h)};
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

        // Try Direct2D rendering path
        if (g_d2d_available && ensure_d2d_render_target()) {
            // Bitmap upload happens in try_update_frame(); just draw here
            g_d2d_rt->BeginDraw();
            g_d2d_rt->Clear(D2D1::ColorF(0.12f, 0.12f, 0.12f));
            if (g_d2d_bitmap && g_d2d_bitmap_current) {
                // Use display dims (g_screen_w/h) for aspect, NOT bitmap dims (g_d2d_bmp_w/h):
                // bitmap is linesize-wide (includes padding), would distort aspect.
                double aspect = (g_screen_h > 0) ? (double)g_screen_w / g_screen_h
                                                  : (double)g_d2d_bmp_w / g_d2d_bmp_h;
                int draw_w, draw_h, off_x, off_y;
                if ((double)cw / ch > aspect) {
                    draw_h = ch;
                    draw_w = (int)(ch * aspect);
                } else {
                    draw_w = cw;
                    draw_h = (int)(cw / aspect);
                }
                if (draw_w == cw - 1) draw_w = cw;
                if (draw_h == ch - 1) draw_h = ch;
                off_x = (cw - draw_w) / 2;
                off_y = (ch - draw_h) / 2;

                D2D1_RECT_F drawRect = D2D1::RectF(
                    static_cast<FLOAT>(off_x), static_cast<FLOAT>(off_y),
                    static_cast<FLOAT>(off_x + draw_w), static_cast<FLOAT>(off_y + draw_h));
                // srcRect crops bitmap (linesize-wide) to display area, discarding padding.
                D2D1_RECT_F srcRect = D2D1::RectF(0, 0,
                    static_cast<FLOAT>(g_screen_w), static_cast<FLOAT>(g_screen_h));
                g_d2d_rt->DrawBitmap(g_d2d_bitmap, drawRect, 1.0f,
                                      D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcRect);
            }

            // Layout highlight overlay (D2D)
            if (LayoutWindow::is_visible()) {
                int hl, ht, hr, hb;
                if (LayoutWindow::get_highlight_bounds(hl, ht, hr, hb) && g_screen_w > 0 && g_screen_h > 0) {
                    double aspect = (double)g_screen_w / g_screen_h;
                    int dw, dh, ox, oy;
                    if ((double)cw / ch > aspect) {
                        dh = ch; dw = (int)(ch * aspect);
                    } else {
                        dw = cw; dh = (int)(cw / aspect);
                    }
                    if (dw == cw - 1) dw = cw;
                    if (dh == ch - 1) dh = ch;
                    ox = (cw - dw) / 2;
                    oy = (ch - dh) / 2;

                    uint32_t phy_w = g_screen_w * g_capture_config.scale;
                    uint32_t phy_h = g_screen_h * g_capture_config.scale;
                    float sx = (float)dw / phy_w;
                    float sy = (float)dh / phy_h;
                    D2D1_RECT_F hlRect = D2D1::RectF(
                        ox + hl * sx, oy + ht * sy,
                        ox + hr * sx, oy + hb * sy);

                    if (!g_d2d_hl_fill)
                        g_d2d_rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.47f, 0.84f, 0.25f), &g_d2d_hl_fill);
                    if (g_d2d_hl_fill) g_d2d_rt->FillRectangle(hlRect, g_d2d_hl_fill);

                    if (!g_d2d_hl_border)
                        g_d2d_rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.47f, 0.84f, 0.9f), &g_d2d_hl_border);
                    if (g_d2d_hl_border) g_d2d_rt->DrawRectangle(hlRect, g_d2d_hl_border, 2.0f);
                }
            }

            // Thin gray separator line at the very top of the canvas to
            // distinguish it from the light toolbar when the streamed image
            // is bright/white and fills the whole client area.
            if (!g_d2d_sep_brush)
                g_d2d_rt->CreateSolidColorBrush(D2D1::ColorF(0.63f, 0.63f, 0.63f, 1.0f), &g_d2d_sep_brush);
            if (g_d2d_sep_brush)
                g_d2d_rt->FillRectangle(D2D1::RectF(0, 0, static_cast<FLOAT>(cw), 1.0f), g_d2d_sep_brush);

            HRESULT hr = g_d2d_rt->EndDraw();
            if (FAILED(hr)) {
                LOGW("D2D EndDraw failed (hr=0x%08X)", (unsigned)hr);
                if (g_d2d_bitmap) { g_d2d_bitmap->Release(); g_d2d_bitmap = nullptr; g_d2d_bitmap_current = false; g_d2d_bmp_w = 0; g_d2d_bmp_h = 0; }
                if (g_d2d_hl_fill) { g_d2d_hl_fill->Release(); g_d2d_hl_fill = nullptr; }
                if (g_d2d_hl_border) { g_d2d_hl_border->Release(); g_d2d_hl_border = nullptr; }
                if (g_d2d_sep_brush) { g_d2d_sep_brush->Release(); g_d2d_sep_brush = nullptr; }
                g_d2d_rt->Release();
                g_d2d_rt = nullptr;
            }
        } else {
            // GDI fallback
            if (ensure_backbuffer(hdc, cw, ch)) {
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
                        if (draw_w == cw - 1) draw_w = cw;
                        if (draw_h == ch - 1) draw_h = ch;
                        off_x = (cw - draw_w) / 2;
                        off_y = (ch - draw_h) / 2;

                        BITMAPINFOHEADER bih = {};
                        bih.biSize     = sizeof(bih);
                        bih.biWidth    = static_cast<LONG>(g_cur_frame->stride / 4);
                        bih.biHeight   = -static_cast<LONG>(g_cur_frame->height);
                        bih.biPlanes   = 1;
                        bih.biBitCount = 32;
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

                // Layout highlight overlay (GDI)
                if (LayoutWindow::is_visible() && g_screen_w > 0 && g_screen_h > 0) {
                    int hl, ht, hr, hb;
                    if (LayoutWindow::get_highlight_bounds(hl, ht, hr, hb)) {
                        double aspect = (double)g_screen_w / g_screen_h;
                        int dw, dh, ox, oy;
                        if ((double)cw / ch > aspect) {
                            dh = ch; dw = (int)(ch * aspect);
                        } else {
                            dw = cw; dh = (int)(cw / aspect);
                        }
                        if (dw == cw - 1) dw = cw;
                        if (dh == ch - 1) dh = ch;
                        ox = (cw - dw) / 2;
                        oy = (ch - dh) / 2;

                        uint32_t phy_w = g_screen_w * g_capture_config.scale;
                        uint32_t phy_h = g_screen_h * g_capture_config.scale;
                        double sx = (double)dw / phy_w;
                        double sy = (double)dh / phy_h;
                        RECT hlRect = {
                            (LONG)(ox + hl * sx), (LONG)(oy + ht * sy),
                            (LONG)(ox + hr * sx), (LONG)(oy + hb * sy)
                        };

                        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 120, 214));
                        HPEN oldPen = (HPEN)SelectObject(g_backbuffer_dc, pen);
                        HBRUSH oldBr = (HBRUSH)SelectObject(g_backbuffer_dc, GetStockObject(NULL_BRUSH));
                        Rectangle(g_backbuffer_dc, hlRect.left, hlRect.top, hlRect.right, hlRect.bottom);
                        SelectObject(g_backbuffer_dc, oldPen);
                        SelectObject(g_backbuffer_dc, oldBr);
                        DeleteObject(pen);
                    }
                }

                BitBlt(hdc, 0, 0, cw, ch, g_backbuffer_dc, 0, 0, SRCCOPY);

                // Thin gray separator line at the top of the canvas
                // (mirrors the D2D path; distinguishes canvas from toolbar).
                RECT sep = {0, 0, cw, 1};
                HBRUSH sepBr = CreateSolidBrush(RGB(160, 160, 160));
                FillRect(hdc, &sep, sepBr);
                DeleteObject(sepBr);
            }
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        if (g_mirroring && g_input) {
            HWND prev = SetFocus(hwnd);
            LOGI("SetFocus to canvas: hwnd=%p, prev=%p, err=%d", hwnd, prev, GetLastError());
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            auto [x, y] = canvas_to_screen(pt.x, pt.y);
            g_mouse_down = true;
            g_last_touch_x = x; g_last_touch_y = y;
            g_input->queue_touch_down(x, y);
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_mouse_down && g_mirroring && g_input) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            // Mouse left the image area — auto touchUp
            if (!is_inside_canvas_image(pt.x, pt.y)) {
                g_mouse_down = false;
                ReleaseCapture();
                g_input->queue_touch_up(g_last_touch_x, g_last_touch_y);
                return 0;
            }
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
            g_input->queue_touch_up(x, y);
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

// Track background threads to avoid detach
static std::thread g_cleanup_thread;
static std::thread g_startup_thread;

static void join_cleanup_thread() {
    if (g_cleanup_thread.joinable()) g_cleanup_thread.join();
}
static void join_startup_thread() {
    if (g_startup_thread.joinable()) g_startup_thread.join();
}

static void join_record_thread() {
    if (g_record_thread.joinable()) g_record_thread.join();
}

// --- Recording / screenshot helpers ---

// YYYYMMDD_HHMMSS
static std::string make_timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

// Open a file with the default associated application.
static void open_file_default(const std::string &path) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, NULL, 0);
    if (wlen <= 0) return;
    std::wstring wpath(wlen - 1, 0);
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &wpath[0], wlen);
    ShellExecuteW(NULL, L"open", wpath.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

// Parse mediatool query output. Sets uri_out (file://media/...) when present,
// or path_out (absolute /path) when the device returns a directly-downloadable path.
// Returns false if no result found.
static bool parse_media_query(const std::string &out, std::string &uri_out, std::string &path_out) {
    if (out.find("find 0 result") != std::string::npos) return false;
    std::istringstream ss(out);
    std::string line;
    bool want_next = false;
    bool want_uri = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "uri") { want_next = true; want_uri = true; continue; }
        if (line == "path") { want_next = true; want_uri = false; continue; }
        if (want_next) {
            if (line.empty()) continue;
            std::string v = line;
            if (v.front() == '"') v.erase(0, 1);
            if (!v.empty() && v.back() == '"') v.pop_back();
            if (!v.empty()) {
                if (want_uri) uri_out = v; else path_out = v;
                return true;
            }
            want_next = false;
        }
    }
    // Fallback: scan for file://media/ anywhere
    size_t u = out.find("file://media/");
    if (u != std::string::npos) {
        std::string v = out.substr(u);
        size_t end = v.find_first_of("\"\r\n");
        if (end != std::string::npos) v = v.substr(0, end);
        if (!v.empty()) { uri_out = v; return true; }
    }
    return false;
}

// Start phone-side screen recording via aa start screenrecorder.
static void start_recording() {
    if (!g_hdc) {
        center_msgbox("Start mirroring first to record.", "HScrcpy", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_recording) { set_status("Already recording"); return; }
    g_record_name = "hscrcpy_" + make_timestamp() + ".mp4";
    std::string serial = g_hdc->serial();
    g_record_serial = serial;
    std::string name = g_record_name;
    join_record_thread();
    g_recording = true;
    set_status("Recording started");
    g_record_thread = std::thread([serial, name]() {
        HdcWrapper hdc(serial);
        std::string out;
        std::string cmd = "aa start -b com.huawei.hmos.screenrecorder "
                          "-a com.huawei.hmos.screenrecorder.ServiceExtAbility "
                          "--ps \"CustomizedFileName\" \"" + name + "\"";
        bool ok = hdc.shell_command(cmd, &out, 15) &&
                  out.find("start ability successfully") != std::string::npos;
        LOGI("record start: ok=%d out=%s", (int)ok, out.c_str());
        PostMessageA(g_hwnd, kMsgRecordStarted, ok ? 1 : 0, 0);
    });
}

// Stop recording. If download_and_open: pull file to Downloads and open it.
// Otherwise (exit cleanup): delete the media library entry, no download.
static void stop_recording(bool download_and_open) {
    if (!g_recording) return;
    KillTimer(g_hwnd, kRecordTimerId);
    std::string name = g_record_name;
    std::string serial = g_record_serial;  // captured at start, survives g_hdc deletion
    join_record_thread();
    set_status(download_and_open ? "Stopping recording..." : "Cleaning up recording...");
    g_record_thread = std::thread([serial, name, download_and_open]() {
        HdcWrapper hdc(serial);
        std::string out;
        static const char *kStopCmd =
            "aa start -b com.huawei.hmos.screenrecorder "
            "-a com.huawei.hmos.screenrecorder.ServiceExtAbility";
        hdc.shell_command(kStopCmd, &out, 15);
        LOGI("record stop: out=%s", out.c_str());

        std::string qcmd = "mediatool query " + name + " -u";
        std::string qout;
        hdc.shell_command(qcmd, &qout, 10);
        LOGI("record query: out=%s", qout.c_str());
        std::string uri, direct_path;
        bool found = parse_media_query(qout, uri, direct_path);

        if (!download_and_open) {
            // Exit cleanup: just delete from media library.
            if (found && !uri.empty()) {
                hdc.shell_command("mediatool delete \"" + uri + "\"", nullptr, 10);
                LOGI("record exit cleanup: deleted uri=%s", uri.c_str());
            }
            PostMessageA(g_hwnd, kMsgRecordStopped, 0, 0);
            return;
        }

        if (!found) {
            LOGE("record: no file found for %s", name.c_str());
            PostMessageA(g_hwnd, kMsgRecordStopped, 0, 0);
            return;
        }

        std::string record_file;
        if (!uri.empty()) {
            // Dest dir MUST have trailing slash — cloud phone mediatool
            // is strict about path format; without the slash it reports
            // "[FAIL] open source media file failed." (misleading error).
            // Also: screenrecorder finalizes the file asynchronously after
            // aa stop returns (close handles, flush moov, release lock).
            // mediatool query finds the entry (already registered) but recv
            // fails because the file is still locked. Retry with delay.
            std::string rcmd = "mediatool recv \"" + uri + "\" /data/local/tmp/";
            std::string rout;
            for (int attempt = 1; attempt <= 3; attempt++) {
                rout.clear();
                hdc.shell_command(rcmd, &rout, 30);
                LOGI("record recv (attempt %d): out=%s", attempt, rout.c_str());
                if (rout.find("[FAIL]") == std::string::npos) break;
                if (attempt < 3) Sleep(3000);
            }
            // Dest path is predictable: mediatool recv copies to
            // /data/local/tmp/<source_basename>, where basename = the
            // CustomizedFileName we passed to aa start. Don't parse recv
            // output — hdc shell (non-interactive) sometimes drops the
            // result path line, leaving only "Table Name: Photos".
            record_file = "/data/local/tmp/" + name;
        } else {
            record_file = direct_path;
        }
        if (record_file.empty()) {
            LOGE("record: could not resolve pullable path for %s", name.c_str());
            PostMessageA(g_hwnd, kMsgRecordStopped, 0, 0);
            return;
        }

        std::string local_path = hscrcpy::get_download_dir() + name;
        std::string pout;
        bool ok = hdc.pull_file(record_file, local_path, &pout);
        LOGI("record pull: remote=%s local=%s ok=%d out=%s",
             record_file.c_str(), local_path.c_str(), (int)ok, pout.c_str());

        // Clean /data/local/tmp copy (best-effort).
        if (!uri.empty()) hdc.shell_command("rm " + record_file, nullptr, 5);
        // Delete media library entry (best-effort).
        if (!uri.empty()) hdc.shell_command("mediatool delete \"" + uri + "\"", nullptr, 10);

        if (ok) {
            g_record_local_path = local_path;
            PostMessageA(g_hwnd, kMsgRecordStopped, 1, 0);
        } else {
            PostMessageA(g_hwnd, kMsgRecordStopped, 0, 0);
        }
    });
}

// Take a single screenshot via snapshot_display, pull to Downloads, open.
static void take_screenshot() {
    if (!g_hdc) {
        center_msgbox("Start mirroring first to take screenshot.", "HScrcpy", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::string serial = g_hdc->serial();
    std::string ts = make_timestamp();
    set_status("Taking screenshot...");
    // Value capture only (serial, ts) — no this; detach is safe.
    std::thread([serial, ts]() {
        HdcWrapper hdc(serial);
        std::string out;
        static const char *kRemote = "/data/local/tmp/hscrcpy_shot.jpeg";
        hdc.shell_command("rm /data/local/tmp/hscrcpy_shot.jpeg", nullptr, 5);
        std::string cmd = "snapshot_display -f /data/local/tmp/hscrcpy_shot.jpeg";
        bool ok = hdc.shell_command(cmd, &out, 15);
        LOGI("screenshot: ok=%d out=%s", (int)ok, out.c_str());
        if (!ok) { PostMessageA(g_hwnd, kMsgScreenshotDone, 0, 0); return; }
        std::string local_path = hscrcpy::get_download_dir() + "hscrcpy_shot_" + ts + ".jpeg";
        std::string pout;
        ok = hdc.pull_file(kRemote, local_path, &pout);
        LOGI("screenshot pull: local=%s ok=%d out=%s",
             local_path.c_str(), (int)ok, pout.c_str());
        hdc.shell_command("rm /data/local/tmp/hscrcpy_shot.jpeg", nullptr, 5);
        if (ok) {
            g_screenshot_local_path = local_path;
            PostMessageA(g_hwnd, kMsgScreenshotDone, 1, 0);
        } else {
            PostMessageA(g_hwnd, kMsgScreenshotDone, 0, 0);
        }
    }).detach();
}

static std::string dump_layout_json() {
    if (!g_hdc) return "";

    std::string output;
    if (!g_hdc->shell_command("uitest dumpLayout", &output, 30)) {
        LOGE("uitest dumpLayout shell command failed");
        return "";
    }

    // Parse output: "DumpLayout saved to: /data/local/tmp/dumpLayout_xxx.json"
    std::string device_path;
    auto pos = output.find("DumpLayout saved to:");
    if (pos != std::string::npos) {
        device_path = output.substr(pos + 21);
        while (!device_path.empty() && (device_path.front() == ' ' || device_path.front() == '\t'))
            device_path.erase(device_path.begin());
        while (!device_path.empty() && (device_path.back() == '\r' || device_path.back() == '\n' || device_path.back() == ' '))
            device_path.pop_back();
    }
    if (device_path.empty()) {
        LOGE("Could not parse dumpLayout output: %s", output.c_str());
        return "";
    }

    char temp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_dir);
    std::string local_path = std::string(temp_dir) + "hscrcpy_layout.json";

    if (!g_hdc->pull_file(device_path, local_path)) {
        LOGE("Failed to pull layout file: %s", device_path.c_str());
        return "";
    }

    // Clean up device-side dump file
    g_hdc->shell_command("rm -f " + device_path, nullptr, 5);

    FILE *f = fopen(local_path.c_str(), "rb");
    if (!f) {
        LOGE("Failed to open local layout file: %s", local_path.c_str());
        return "";
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string json_str(len, '\0');
    size_t read = fread(&json_str[0], 1, len, f);
    json_str.resize(read);
    fclose(f);

    DeleteFileA(local_path.c_str());
    return json_str;
}

static void stop_mirroring();

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
                PostMessageA(g_hwnd, kMsgLayoutUpdate, 0, 0);
            }
            {
                std::lock_guard<std::mutex> lock(g_frame_mutex);
                g_cur_frame = std::move(img);
                g_d2d_bitmap_current = false;
            }

            // Upload to D2D bitmap now (once per new frame, on UI thread)
            if (g_d2d_available && ensure_d2d_render_target()) {
                std::lock_guard<std::mutex> lock(g_frame_mutex);
                if (g_cur_frame && !g_cur_frame->data.empty() && g_cur_frame->width > 0) {
                    upload_frame_to_d2d(*g_cur_frame);
                    g_d2d_bitmap_current = true;
                }
            }

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
        g_d2d_bitmap_current = false;
    }
    // Release D2D bitmap so WM_PAINT paints black, not stale frame
    if (g_d2d_bitmap) {
        g_d2d_bitmap->Release();
        g_d2d_bitmap = nullptr;
        g_d2d_bmp_w = 0;
        g_d2d_bmp_h = 0;
    }

    {
        HICON hPrev = (HICON)SendMessageA(g_btn_start_hwnd, BM_GETIMAGE, IMAGE_ICON, 0);
        HICON hStartIcon = load_png_icon("res\\toolbar\\start.png", 24, 85, 85, 85, true);
        SendMessageA(g_btn_start_hwnd, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hStartIcon);
        if (hPrev) DestroyIcon(hPrev);
    }
    EnableWindow(g_btn_start_hwnd, TRUE);
    set_status("Stopping...");
    g_snapshot_warned = false;

    // Hide layout panel on stop
    if (g_layout_visible) {
        LayoutWindow::hide();
        ShowWindow(g_splitter_hwnd, SW_HIDE);
        g_layout_visible = false;
        // Shrink window: remove panel width
        RECT wr;
        GetWindowRect(g_hwnd, &wr);
        SetWindowPos(g_hwnd, NULL, 0, 0,
                     wr.right - wr.left - LayoutWindow::panel_width(),
                     wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }

    // Hide file manager panel on stop
    if (g_fm_visible) {
        FileManagerWindow::hide();
        ShowWindow(g_splitter_hwnd, SW_HIDE);
        g_fm_visible = false;
        RECT wr;
        GetWindowRect(g_hwnd, &wr);
        SetWindowPos(g_hwnd, NULL, 0, 0,
                     wr.right - wr.left - FileManagerWindow::panel_width(),
                     wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }

    // Hide CPH panel on stop
    if (g_cph_visible) {
        CphWindow::hide();
        ShowWindow(g_splitter_hwnd, SW_HIDE);
        g_cph_visible = false;
        RECT wr;
        GetWindowRect(g_hwnd, &wr);
        SetWindowPos(g_hwnd, NULL, 0, 0,
                     wr.right - wr.left - CphWindow::panel_width(),
                     wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }
    CphWindow::reset();

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
        PostMessageA(g_hwnd, kMsgStopComplete, 0, 0);
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
                PostMessageA(g_hwnd, kMsgStartFailed, 0, 0);
                return;
            }
        }
        // Secondary: quick echo to verify shell access
        if (!g_hdc->shell_command("echo ok", nullptr, 3)) {
            LOGE("Device %s not reachable", serial.c_str());
            PostMessageA(g_hwnd, kMsgStartFailed, 0, 0);
            return;
        }

        auto_select_config();

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
        PostMessageA(g_hwnd, kMsgConnectingStatus, 0, 0);

        bool ok = false;
        try {
            ok = g_capture->start();
        } catch (const std::exception &e) {
            LOGE("g_capture->start() exception: %s", e.what());
            ok = false;
        }

        if (!ok) {
            PostMessageA(g_hwnd, kMsgStartFailed, 0, 0);
            return;
        }

        HANDLE evt = g_capture->frame_event();
        for (int i = 0; i < 300 && g_capture->width() == 0; i++) {
            if (evt) {
                WaitForSingleObject(evt, 100);
            } else {
                Sleep(100);
            }
        }

        if (g_capture->width() == 0) {
            PostMessageA(g_hwnd, kMsgNoFrame, 0, 0);
            return;
        }

        PostMessageA(g_hwnd, kMsgCaptureReady, 0, 0);
    });
}

static LRESULT CALLBACK SplitterWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_SIZEWE));
        return TRUE;
    case WM_LBUTTONDOWN: {
        g_splitter_dragging = true;
        POINT pt;
        GetCursorPos(&pt);
        RECT sr;
        GetWindowRect(hwnd, &sr);
        g_splitter_drag_offset = pt.x - sr.left;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_splitter_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            int splitter_screen_x = pt.x - g_splitter_drag_offset;
            POINT sp = {splitter_screen_x, 0};
            ScreenToClient(g_hwnd, &sp);
            RECT rc;
            GetClientRect(g_hwnd, &rc);
            int new_pw = rc.right - sp.x;
            if (new_pw < 200) new_pw = 200;
            int canvas_w = rc.right - new_pw;
            if (canvas_w < 100) { canvas_w = 100; new_pw = rc.right - canvas_w; }
            int client_h = rc.bottom - TOOLBAR_H;
            if (g_layout_visible) {
                LayoutWindow::reposition(canvas_w, TOOLBAR_H, new_pw, client_h);
            } else if (g_fm_visible) {
                FileManagerWindow::reposition(canvas_w, TOOLBAR_H, new_pw, client_h);
            } else if (g_cph_visible) {
                CphWindow::reposition(canvas_w, TOOLBAR_H, new_pw, client_h);
            }
            MoveWindow(g_splitter_hwnd, canvas_w - 2, TOOLBAR_H, 4, client_h, TRUE);
            MoveWindow(g_canvas_hwnd, 0, TOOLBAR_H, canvas_w, client_h, FALSE);
            destroy_backbuffer();
            if (g_d2d_rt) g_d2d_rt->Resize(D2D1::SizeU(canvas_w, client_h));
            InvalidateRect(g_canvas_hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (g_splitter_dragging) {
            g_splitter_dragging = false;
            ReleaseCapture();
            if (g_layout_visible) {
                RECT lr;
                GetClientRect(LayoutWindow::hwnd(), &lr);
                LayoutWindow::set_panel_width(lr.right);
            } else if (g_fm_visible) {
                RECT fr;
                GetClientRect(FileManagerWindow::hwnd(), &fr);
                FileManagerWindow::set_panel_width(fr.right);
            } else if (g_cph_visible) {
                RECT cr;
                GetClientRect(CphWindow::hwnd(), &cr);
                CphWindow::set_panel_width(cr.right);
            }
            return 0;
        }
        break;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Row 1: Device label + combo + Refresh/Start icon buttons
        CreateWindowA("STATIC", "Device:",
                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                       8, 10, 48, 18, hwnd, NULL, g_hInst, NULL);

        g_combo_hwnd = CreateWindowA("COMBOBOX", "",
                                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      58, 6, 197, 200, hwnd, (HMENU)ID_COMBO, g_hInst, NULL);

        g_btn_refresh_hwnd = CreateWindowA("BUTTON", "",
                                            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_ICON,
                                            259, 5, 28, 28, hwnd, (HMENU)ID_BTN_REFRESH, g_hInst, NULL);
        HICON hRefresh = load_png_icon("res\\toolbar\\refresh.png", 24, 85, 85, 85, true);
        if (hRefresh) SendMessageA(g_btn_refresh_hwnd, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hRefresh);

        g_btn_start_hwnd = CreateWindowA("BUTTON", "",
                                           WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_ICON | WS_DISABLED,
                                           291, 5, 28, 28, hwnd, (HMENU)ID_BTN_START, g_hInst, NULL);
        HICON hStart = load_png_icon("res\\toolbar\\start.png", 24, 85, 85, 85, true);
        if (hStart) SendMessageA(g_btn_start_hwnd, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hStart);

        SetWindowTheme(g_btn_refresh_hwnd, L"", L"");
        SetWindowTheme(g_btn_start_hwnd, L"", L"");

        // Status text on Row 1, right of Start/Stop button (ends at x=320).
        // 4px gap between button and text.
        g_status_hwnd = CreateWindowA("STATIC", "",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       323, 10, 400, 18, hwnd, NULL, g_hInst, NULL);

        // Row 2: icon buttons via mkbtn lambda
        int bx = 6, by = 35, bsz = 28, bgap = 4;
        auto mkbtn = [&](const char *png, int id) -> HWND {
            HWND h = CreateWindowA("BUTTON", "",
                                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_ICON,
                                    bx, by, bsz, bsz, hwnd, (HMENU)(INT_PTR)id, g_hInst, NULL);
            HICON ico = load_png_icon(png, 24, 85, 85, 85, true);
            if (ico) SendMessageA(h, BM_SETIMAGE, IMAGE_ICON, (LPARAM)ico);
            SetWindowTheme(h, L"", L"");
            bx += bsz + bgap;
            return h;
        };

        g_btn_quality_hwnd = mkbtn("res\\toolbar\\settings.png", ID_BTN_QUALITY);
        g_btn_layout_hwnd  = mkbtn("res\\toolbar\\layout.png", ID_BTN_LAYOUT);
        g_btn_fm_hwnd      = mkbtn("res\\toolbar\\file-manager.png", ID_BTN_FM);
        g_btn_cph_hwnd     = mkbtn("res\\toolbar\\cloud-phone.png", ID_BTN_CPH);
        g_btn_back_hwnd    = mkbtn("res\\toolbar\\back.png", ID_BTN_BACK);
        g_btn_home_hwnd    = mkbtn("res\\toolbar\\home.png", ID_BTN_HOME);
        g_btn_recent_hwnd  = mkbtn("res\\toolbar\\recent.png", ID_BTN_RECENT);
        g_btn_volup_hwnd   = mkbtn("res\\toolbar\\vol-increase.png", ID_BTN_VOLUP);
        g_btn_voldown_hwnd = mkbtn("res\\toolbar\\vol-decrease.png", ID_BTN_VOLDOWN);
        g_btn_power_hwnd   = mkbtn("res\\toolbar\\power.png", ID_BTN_POWER);
        g_btn_reboot_hwnd  = mkbtn("res\\toolbar\\reboot.png", ID_BTN_REBOOT);
        g_btn_record_hwnd  = mkbtn("res\\toolbar\\record.png", ID_BTN_RECORD);
        g_btn_screenshot_hwnd = mkbtn("res\\toolbar\\screenshot.png", ID_BTN_SCREENSHOT);

        g_canvas_hwnd = CreateWindowA(CANVAS_CLASS, "",
                                       WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_TABSTOP,
                                       0, TOOLBAR_H, 400, 400, hwnd, NULL, g_hInst, NULL);

        g_canvas_orig_wndproc = (WNDPROC)SetWindowLongPtrA(g_canvas_hwnd, GWLP_WNDPROC, (LONG_PTR)CanvasKeyProc);
        LOGI("Canvas subclassed, orig_wndproc=%p, err=%d", g_canvas_orig_wndproc, GetLastError());

        LayoutWindow::create(g_hInst, hwnd, 420);
        FileManagerWindow::create(g_hInst, hwnd, 420);
        CphWindow::create(g_hInst, hwnd, 420);

        // Register and create splitter bar (hidden initially)
        {
            static bool reg = false;
            if (!reg) {
                WNDCLASSEXA swc = {};
                swc.cbSize = sizeof(swc);
                swc.style = CS_HREDRAW | CS_VREDRAW;
                swc.lpfnWndProc = SplitterWndProc;
                swc.hInstance = g_hInst;
                swc.hCursor = LoadCursor(NULL, IDC_SIZEWE);
                swc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
                swc.lpszClassName = "HScrcpySplitterClass";
                RegisterClassExA(&swc);
                reg = true;
            }
            g_splitter_hwnd = CreateWindowA("HScrcpySplitterClass", "",
                                             WS_CHILD | WS_VISIBLE,
                                             0, TOOLBAR_H, 4, 100, hwnd, NULL, g_hInst, NULL);
            ShowWindow(g_splitter_hwnd, SW_HIDE);
        }

        refresh_devices();
        break;
    }
    case WM_LBUTTONUP:
        // Fallback: if mouse was down on canvas but BUTTONUP arrived at main
        // window instead (e.g. SetCapture lost capture), still send touchUp
        if (g_mouse_down && g_mirroring && g_input) {
            g_mouse_down = false;
            ReleaseCapture();
            g_input->queue_touch_up(g_last_touch_x, g_last_touch_y);
            return 0;
        }
        break;
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
                    } else if (!g_starting) {
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
        case ID_BTN_RECORD:
            if (g_recording) stop_recording(true);
            else start_recording();
            break;
        case ID_BTN_SCREENSHOT:
            take_screenshot();
            break;
        case ID_BTN_QUALITY:
            show_settings_dialog();
            break;
        case ID_BTN_LAYOUT:
            if (!g_mirroring) {
                center_msgbox("Start mirroring first to inspect layout.", "HScrcpy", MB_OK | MB_ICONINFORMATION);
                break;
            }
            if (LayoutWindow::is_visible()) {
                // Hide layout panel
                LayoutWindow::hide();
                ShowWindow(g_splitter_hwnd, SW_HIDE);
                g_layout_visible = false;
                if (IsZoomed(hwnd)) {
                    SendMessageW(hwnd, WM_SIZE, 0, 0);
                } else {
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    SetWindowPos(hwnd, NULL, 0, 0,
                                 wr.right - wr.left - LayoutWindow::panel_width(),
                                 wr.bottom - wr.top,
                                 SWP_NOMOVE | SWP_NOZORDER);
                }
            } else {
                bool need_expand = !g_layout_visible && !g_fm_visible && !g_cph_visible;
                // Hide FM panel if visible
                if (g_fm_visible) {
                    FileManagerWindow::hide();
                    g_fm_visible = false;
                }
                // Hide CPH panel if visible
                if (g_cph_visible) {
                    CphWindow::hide();
                    g_cph_visible = false;
                }
                LayoutWindow::show(
                    [](int left, int top, int right, int bottom) {
                        InvalidateRect(g_canvas_hwnd, NULL, FALSE);
                    },
                    []() -> std::string {
                        return dump_layout_json();
                    }
                );
                ShowWindow(g_splitter_hwnd, SW_SHOW);
                g_layout_visible = true;
                if (need_expand && !IsZoomed(hwnd)) {
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    SetWindowPos(hwnd, NULL, 0, 0,
                                 wr.right - wr.left + LayoutWindow::panel_width(),
                                 wr.bottom - wr.top,
                                 SWP_NOMOVE | SWP_NOZORDER);
                } else {
                    // Panel swapped or maximized — just reposition
                    SendMessageW(hwnd, WM_SIZE, 0, 0);
                }
            }
            break;
        case ID_BTN_FM:
            if (!g_mirroring) {
                center_msgbox("Start mirroring first to use file manager.", "HScrcpy", MB_OK | MB_ICONINFORMATION);
                break;
            }
            if (FileManagerWindow::is_visible()) {
                // Hide FM panel
                FileManagerWindow::hide();
                ShowWindow(g_splitter_hwnd, SW_HIDE);
                g_fm_visible = false;
                if (IsZoomed(hwnd)) {
                    SendMessageW(hwnd, WM_SIZE, 0, 0);
                } else {
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    SetWindowPos(hwnd, NULL, 0, 0,
                                 wr.right - wr.left - FileManagerWindow::panel_width(),
                                 wr.bottom - wr.top,
                                 SWP_NOMOVE | SWP_NOZORDER);
                }
            } else {
                bool need_expand = !g_layout_visible && !g_fm_visible && !g_cph_visible;
                // Hide layout panel if visible
                if (g_layout_visible) {
                    LayoutWindow::hide();
                    g_layout_visible = false;
                }
                // Hide CPH panel if visible
                if (g_cph_visible) {
                    CphWindow::hide();
                    g_cph_visible = false;
                }
                FileManagerWindow::show(
                    [](const std::string &cmd, std::string *output, int timeout_sec) -> bool {
                        if (!g_hdc) return false;
                        return g_hdc->shell_command(cmd, output, timeout_sec);
                    },
                    [](const std::string &local, const std::string &remote, std::string *output) -> bool {
                        if (!g_hdc) return false;
                        return g_hdc->push_file(local, remote, output);
                    },
                    [](const std::string &remote, const std::string &local, std::string *output) -> bool {
                        if (!g_hdc) return false;
                        return g_hdc->pull_file(remote, local, output);
                    },
                    [](bool busy) {
                    }
                );
                ShowWindow(g_splitter_hwnd, SW_SHOW);
                g_fm_visible = true;
                if (need_expand && !IsZoomed(hwnd)) {
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    SetWindowPos(hwnd, NULL, 0, 0,
                                 wr.right - wr.left + FileManagerWindow::panel_width(),
                                 wr.bottom - wr.top,
                                 SWP_NOMOVE | SWP_NOZORDER);
                } else {
                    // Panel swapped or maximized — just reposition
                    SendMessageW(hwnd, WM_SIZE, 0, 0);
                }
            }
            break;
        case ID_BTN_CPH:
            if (!g_mirroring) {
                center_msgbox("Start mirroring first to use cloud phone simulation.", "HScrcpy", MB_OK | MB_ICONINFORMATION);
                break;
            }
            // Cloud phone detection: cached at mirroring start
            {
                if (!g_is_cloud_phone) {
                    center_msgbox("This device does not support cloud phone simulation.", "HScrcpy", MB_OK | MB_ICONWARNING);
                    break;
                }
            }
            if (CphWindow::is_visible()) {
                CphWindow::hide();
                ShowWindow(g_splitter_hwnd, SW_HIDE);
                g_cph_visible = false;
                if (IsZoomed(hwnd)) {
                    SendMessageW(hwnd, WM_SIZE, 0, 0);
                } else {
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    SetWindowPos(hwnd, NULL, 0, 0,
                                 wr.right - wr.left - CphWindow::panel_width(),
                                 wr.bottom - wr.top,
                                 SWP_NOMOVE | SWP_NOZORDER);
                }
            } else {
                bool need_expand = !g_layout_visible && !g_fm_visible && !g_cph_visible;
                // Hide other panels
                if (g_layout_visible) { LayoutWindow::hide(); g_layout_visible = false; }
                if (g_fm_visible) { FileManagerWindow::hide(); g_fm_visible = false; }
                CphWindow::show(
                    [](const std::string &cmd, std::string *output, int timeout_sec) -> bool {
                        if (!g_hdc) return false;
                        return g_hdc->shell_command(cmd, output, timeout_sec);
                    },
                    [](const std::string &local_path, const std::string &remote_path) -> bool {
                        if (!g_hdc) return false;
                        return g_hdc->push_file(local_path, remote_path);
                    },
                    [](bool busy) {
                    }
                );
                ShowWindow(g_splitter_hwnd, SW_SHOW);
                g_cph_visible = true;
                if (need_expand && !IsZoomed(hwnd)) {
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    SetWindowPos(hwnd, NULL, 0, 0,
                                 wr.right - wr.left + CphWindow::panel_width(),
                                 wr.bottom - wr.top,
                                 SWP_NOMOVE | SWP_NOZORDER);
                } else {
                    // Panel swapped or maximized — just reposition
                    SendMessageW(hwnd, WM_SIZE, 0, 0);
                }
            }
            break;
        }
        break;
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int client_w = rc.right;
        int client_h = rc.bottom - TOOLBAR_H;

        int canvas_w = client_w;
        if (g_layout_visible && LayoutWindow::is_visible()) {
            int pw = LayoutWindow::panel_width();
            canvas_w = client_w - pw;
            if (canvas_w < 100) { canvas_w = 100; pw = client_w - canvas_w; }
            LayoutWindow::reposition(canvas_w, TOOLBAR_H, pw, client_h);
            MoveWindow(g_splitter_hwnd, canvas_w - 2, TOOLBAR_H, 4, client_h, TRUE);
        } else if (g_fm_visible && FileManagerWindow::is_visible()) {
            int pw = FileManagerWindow::panel_width();
            canvas_w = client_w - pw;
            if (canvas_w < 100) { canvas_w = 100; pw = client_w - canvas_w; }
            FileManagerWindow::reposition(canvas_w, TOOLBAR_H, pw, client_h);
            MoveWindow(g_splitter_hwnd, canvas_w - 2, TOOLBAR_H, 4, client_h, TRUE);
        } else if (g_cph_visible && CphWindow::is_visible()) {
            int pw = CphWindow::panel_width();
            canvas_w = client_w - pw;
            if (canvas_w < 100) { canvas_w = 100; pw = client_w - canvas_w; }
            CphWindow::reposition(canvas_w, TOOLBAR_H, pw, client_h);
            MoveWindow(g_splitter_hwnd, canvas_w - 2, TOOLBAR_H, 4, client_h, TRUE);
        }

        MoveWindow(g_canvas_hwnd, 0, TOOLBAR_H, canvas_w, client_h, FALSE);
        destroy_backbuffer();
        if (g_d2d_rt) g_d2d_rt->Resize(D2D1::SizeU(canvas_w, client_h));
        InvalidateRect(g_canvas_hwnd, NULL, FALSE);
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT*)lp;
        if (dis->CtlType == ODT_BUTTON) {
            FillRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(WHITE_BRUSH));
            HICON icon = (HICON)SendMessageA(dis->hwndItem, BM_GETIMAGE, IMAGE_ICON, 0);
            if (icon) {
                int ox = 2, oy = 2;
                if (dis->itemState & ODS_SELECTED) { ox++; oy++; }
                UINT flags = DST_ICON | ((dis->itemState & ODS_DISABLED) ? DSS_DISABLED : DSS_NORMAL);
                DrawState(dis->hDC, NULL, NULL, (LPARAM)icon, 0,
                          dis->rcItem.left + ox, dis->rcItem.top + oy, 0, 0, flags);
            }
            return TRUE;
        }
        break;
    }
    case WM_ERASEBKGND: {
        // Only erase the toolbar area. The canvas child window covers the
        // region below TOOLBAR_H; filling it here would flash gray before the
        // canvas repaints, causing visible video flicker.
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT tb = {0, 0, rc.right, TOOLBAR_H};
        FillRect(hdc, &tb, (HBRUSH)GetStockObject(WHITE_BRUSH));
        return 1;
    }
    case WM_CTLCOLORBTN: {
        HDC hdcBtn = (HDC)wp;
        SetBkMode(hdcBtn, TRANSPARENT);
        SetTextColor(hdcBtn, RGB(60, 60, 60));
        return (INT_PTR)GetStockObject(WHITE_BRUSH);
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wp;
        SetBkMode(hdcStatic, TRANSPARENT);
        SetTextColor(hdcStatic, RGB(60, 60, 60));
        return (INT_PTR)GetStockObject(WHITE_BRUSH);
    }
    case WM_TIMER:
        if (wp == 1 && g_mirroring) {
            try_update_frame();
            if (!g_cjk_buf.empty() && g_cjk_buf_time > 0 && GetTickCount64() - g_cjk_buf_time >= 100) {
                flush_cjk_buffer();
            }
        }
        if (wp == 3) {
            KillTimer(hwnd, 3);
            if (g_mirroring && g_capture && !IsZoomed(hwnd)) {
                // On portrait↔landscape rotation, swap canvas width and height
                // so the frame fills the canvas without letterbox bars.
                bool frame_portrait = (g_screen_w < g_screen_h);
                RECT rc;
                GetClientRect(hwnd, &rc);
                int client_w = rc.right;
                int client_h = rc.bottom - TOOLBAR_H;
                int panel_w = 0;
                if (g_layout_visible) panel_w = LayoutWindow::panel_width();
                else if (g_fm_visible) panel_w = FileManagerWindow::panel_width();
                else if (g_cph_visible) panel_w = CphWindow::panel_width();
                int canvas_w = client_w - panel_w;
                bool canvas_portrait = (canvas_w < client_h);
                if (frame_portrait != canvas_portrait) {
                    // Swap canvas dimensions; panel width and toolbar stay fixed
                    int new_client_w = client_h + panel_w;
                    int new_client_h = canvas_w + TOOLBAR_H;
                    RECT wr = {0, 0, new_client_w, new_client_h};
                    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
                    SetWindowPos(hwnd, NULL, 0, 0,
                                 wr.right - wr.left, wr.bottom - wr.top,
                                 SWP_NOMOVE | SWP_NOZORDER);
                } else {
                    // Same orientation (e.g. scale change) — just repaint
                    InvalidateRect(g_canvas_hwnd, NULL, FALSE);
                }
            } else if (g_mirroring && g_capture) {
                // Maximized — can't resize, letterbox
                InvalidateRect(g_canvas_hwnd, NULL, FALSE);
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
        if (wp == kRecordTimerId) {
            KillTimer(hwnd, kRecordTimerId);
            if (g_recording) {
                LOGI("Recording auto-stop (30min max)");
                set_status("Recording auto-stopped (30min max)");
                stop_recording(true);
            }
        }
        break;
    case kMsgStartFailed: {
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
    case kMsgNoFrame: {
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
    case kMsgCaptureReady: {
        g_starting = false;
        g_screen_w = g_capture->width();
        g_screen_h = g_capture->height();

        g_input = new InputInjector(*g_hdc, g_capture->local_base_port());
        join_startup_thread();
        g_startup_thread = std::thread([]() {
            if (g_input) g_input->init(g_capture ? g_capture->uitest_version() : "");
        });

        g_mirroring = true;
        g_is_cloud_phone = g_hdc && g_hdc->is_tcp() && CphWindow::is_cloud_phone(
            [](const std::string &cmd, std::string *output, int timeout_sec) -> bool {
                if (!g_hdc) return false;
                return g_hdc->shell_command(cmd, output, timeout_sec);
            });
        LOGE("Cloud phone detection: is_tcp=%d is_cloud_phone=%d serial=%s",
             g_hdc ? g_hdc->is_tcp() : -1, g_is_cloud_phone,
             g_hdc ? g_hdc->serial().c_str() : "null");
        g_capture->set_notify_hwnd(g_hwnd);
        {
            HICON hPrev = (HICON)SendMessageA(g_btn_start_hwnd, BM_GETIMAGE, IMAGE_ICON, 0);
            HICON hStopIcon = load_png_icon("res\\toolbar\\stop.png", 24, 85, 85, 85, true);
            SendMessageA(g_btn_start_hwnd, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hStopIcon);
            if (hPrev) DestroyIcon(hPrev);
        }
        EnableWindow(g_btn_start_hwnd, TRUE);

        // Auto-size window only on first connect per phone serial.
        // Subsequent reconnects to the same phone keep the user's window size;
        // rotation is handled by swapping canvas dimensions (Timer 3), not resize.
        {
            std::string cur_serial = g_hdc ? g_hdc->serial() : "";
            if (g_sized_serial != cur_serial) {
                resize_window_to_frame_aspect(g_hwnd);
                g_sized_serial = cur_serial;
            }
        }

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
    case kMsgStopComplete: {
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
    case kMsgLayoutUpdate: {
        if (!g_mirroring || !g_capture || !g_hdc) break;

        // Debounce rotation repaint: portrait↔landscape can flicker rapidly.
        // SetTimer resets on each call, so only the last change in a burst
        // triggers a repaint. 300ms covers most rotation flicker.
        SetTimer(hwnd, 3, 300, NULL);

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
    case kMsgConnectingStatus: {
        char buf[256];
        snprintf(buf, sizeof(buf), "Connecting... (scale=%d, %s, %dfps)",
                 g_capture_config.scale, bitrate_label(g_capture_config.bit_rate),
                 g_capture_config.frame_rate);
        set_status(buf);
        break;
    }
    case kMsgConnLostStartup: {
        if (g_mirroring && g_hdc) {
            std::string serial = g_hdc->serial();
            std::string ip = g_hdc->ip();
            std::string port = g_hdc->port();
            EnableWindow(g_btn_start_hwnd, FALSE);
            stop_mirroring();
            if (!g_cleanup_serial.empty() && serial == g_cleanup_serial) {
                g_pending_start_serial = serial;
                g_pending_start_ip = ip;
                g_pending_start_port = port;
            }
        }
        break;
    }
    case kMsgDeviceDisconnected: {
        // Device disconnected (hdc unreachable) — stop mirroring and notify user
        if (g_mirroring) {
            show_error("Device disconnected. Please reconnect and try again.");
            EnableWindow(g_btn_start_hwnd, FALSE);
            stop_mirroring();
        }
        break;
    }
    case kMsgFrameRender: {
        // Event-driven frame rendering: decode thread notifies UI immediately
        if (g_mirroring) try_update_frame();
        break;
    }
    case kMsgStreamEnded: {
        // Stream loop ended (connection lost / max retries exceeded)
        // Auto-stop mirroring and update UI so user knows the connection is dead
        if (g_mirroring) {
            LOGI("Stream ended notification received, stopping mirroring");
            stop_mirroring();
        }
        break;
    }
    case kMsgRecordStarted: {
        if (wp) {
            SetTimer(hwnd, kRecordTimerId, kRecordMaxMs, NULL);
            set_status("Recording started (click again to stop)");
        } else {
            g_recording = false;
            set_status("Recording failed to start");
            center_msgbox("Failed to start screen recorder on device.", "HScrcpy", MB_OK | MB_ICONWARNING);
        }
        break;
    }
    case kMsgRecordStopped: {
        g_recording = false;
        if (wp) {
            set_status("Recording saved to Downloads");
            char msg[600];
            snprintf(msg, sizeof(msg), "Recording saved: %s", g_record_local_path.c_str());
            FileManagerWindow::show_toast(msg, true, g_hwnd);
            open_file_default(g_record_local_path);
        } else {
            set_status("Recording stopped (pull failed)");
            FileManagerWindow::show_toast("Recording pull failed", false, g_hwnd);
        }
        break;
    }
    case kMsgScreenshotDone: {
        if (wp) {
            set_status("Screenshot saved to Downloads");
            char msg[600];
            snprintf(msg, sizeof(msg), "Screenshot saved: %s", g_screenshot_local_path.c_str());
            FileManagerWindow::show_toast(msg, true, g_hwnd);
            open_file_default(g_screenshot_local_path);
        } else {
            set_status("Screenshot failed");
            FileManagerWindow::show_toast("Screenshot failed", false, g_hwnd);
        }
        break;
    }
    case WM_DESTROY:
        destroy_backbuffer();
        LayoutWindow::destroy();
        FileManagerWindow::destroy();
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
        KillTimer(hwnd, kRecordTimerId);
        // If recording is active, stop it on the device and clean up the media
        // library entry. Do NOT download — just delete the temp recording.
        // stop_recording(false) spawns the cleanup thread; join_record_thread
        // below waits for it so the process doesn't exit mid-cleanup.
        if (g_recording) {
            stop_recording(false);
        }
        join_record_thread();
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

    init_d2d();

    hscrcpy::config_load();

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
        g_verbose = g_cli_args.verbose;
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

    SetTimer(g_hwnd, 1, 100, NULL);

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
    cleanup_d2d();
    return 0;
}
