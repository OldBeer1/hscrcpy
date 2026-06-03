#ifndef HSCRCPY_WINDOW_RENDERER_H
#define HSCRCPY_WINDOW_RENDERER_H

#include "h264_decoder.h"
#include <cstdint>
#include <string>

#include <windows.h>

namespace hscrcpy {

class WindowRenderer {
public:
    WindowRenderer();
    ~WindowRenderer();

    bool init(uint32_t screen_w, uint32_t screen_h, const char *title = "HScrcpy");
    void cleanup();

    bool render(const DecodedImage &img);
    void toggle_fullscreen();

    int window_width()  const { return win_w_; }
    int window_height() const { return win_h_; }
    HWND hwnd() const { return hwnd_; }

    static WindowRenderer *instance() { return instance_; }

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_  = nullptr;
    int win_w_  = 0;
    int win_h_  = 0;
    uint32_t screen_w_ = 0;
    uint32_t screen_h_ = 0;
    bool fullscreen_ = false;
    DWORD saved_style_ = 0;
    RECT saved_rect_ = {};
    bool initialized_ = false;
    static WindowRenderer *instance_;
};

} // namespace hscrcpy

#endif
