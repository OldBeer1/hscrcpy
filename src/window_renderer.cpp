#include "window_renderer.h"
#include "log.h"
#include <cstring>

namespace hscrcpy {

WindowRenderer *WindowRenderer::instance_ = nullptr;

WindowRenderer::WindowRenderer() {
    instance_ = this;
}

WindowRenderer::~WindowRenderer() {
    cleanup();
    instance_ = nullptr;
}

bool WindowRenderer::init(uint32_t screen_w, uint32_t screen_h, const char *title) {
    screen_w_ = screen_w;
    screen_h_ = screen_h;
    win_w_ = static_cast<int>(screen_w / 2);
    win_h_ = static_cast<int>(screen_h / 2);

    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance      = GetModuleHandle(NULL);
    wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName  = "HScrcpyMirrorClass";
    RegisterClassExA(&wc);

    hwnd_ = CreateWindowExA(0, "HScrcpyMirrorClass", title,
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              win_w_, win_h_,
                              NULL, NULL, wc.hInstance, NULL);
    if (!hwnd_) {
        LOGE("CreateWindow failed");
        return false;
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    initialized_ = true;
    LOGI("Window created: %dx%d", win_w_, win_h_);
    return true;
}

void WindowRenderer::cleanup() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    initialized_ = false;
}

bool WindowRenderer::render(const DecodedImage &img) {
    if (!hwnd_) return false;

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    if (!hdc) return false;

    RECT client_rect;
    GetClientRect(hwnd_, &client_rect);
    int cw = client_rect.right;
    int ch = client_rect.bottom;

    BITMAPINFOHEADER bih = {};
    bih.biSize        = sizeof(bih);
    bih.biWidth       = static_cast<LONG>(img.width);
    bih.biHeight      = -static_cast<LONG>(img.height);
    bih.biPlanes      = 1;
    bih.biBitCount    = 24;
    bih.biCompression = BI_RGB;

    SetStretchBltMode(hdc, HALFTONE);
    StretchDIBits(hdc, 0, 0, cw, ch,
                  0, 0, img.width, img.height,
                  img.data.data(),
                  reinterpret_cast<BITMAPINFO *>(&bih),
                  DIB_RGB_COLORS, SRCCOPY);

    EndPaint(hwnd_, &ps);
    return true;
}

void WindowRenderer::toggle_fullscreen() {
    if (!hwnd_) return;
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        saved_style_ = GetWindowLong(hwnd_, GWL_STYLE);
        GetWindowRect(hwnd_, &saved_rect_);
        SetWindowLong(hwnd_, GWL_STYLE, WS_POPUP);
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED);
    } else {
        SetWindowLong(hwnd_, GWL_STYLE, saved_style_);
        SetWindowPos(hwnd_, NULL, saved_rect_.left, saved_rect_.top,
                     saved_rect_.right - saved_rect_.left,
                     saved_rect_.bottom - saved_rect_.top, SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK WindowRenderer::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_PAINT && instance_ && instance_->initialized_) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace hscrcpy
