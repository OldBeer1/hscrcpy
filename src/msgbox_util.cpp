#include "msgbox_util.h"

namespace hscrcpy {

static HWND g_center_on = nullptr;
static HHOOK g_hook = nullptr;

static LRESULT CALLBACK MsgBoxCenterCBT(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HCBT_ACTIVATE && g_center_on) {
        HWND mb = (HWND)wp;
        RECT pr, mr;
        GetWindowRect(g_center_on, &pr);
        GetWindowRect(mb, &mr);
        int x = pr.left + (pr.right - pr.left - (mr.right - mr.left)) / 2;
        int y = pr.top + (pr.bottom - pr.top - (mr.bottom - mr.top)) / 2;
        SetWindowPos(mb, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        // Unhook immediately so HCBT_ACTIVATE for the parent window re-activation
        // after MessageBox closes does not reposition the parent window.
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    return CallNextHookEx(NULL, nCode, wp, lp);
}

int centered_msgbox_a(HWND parent, const char *text, const char *title, UINT type,
                      HWND center_on) {
    g_center_on = center_on ? center_on : parent;
    g_hook = SetWindowsHookExW(WH_CBT, MsgBoxCenterCBT, NULL, GetCurrentThreadId());
    int ret = MessageBoxA(parent, text, title, type);
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    g_center_on = nullptr;
    return ret;
}

int centered_msgbox_w(HWND parent, const wchar_t *text, const wchar_t *title, UINT type,
                      HWND center_on) {
    g_center_on = center_on ? center_on : parent;
    g_hook = SetWindowsHookExW(WH_CBT, MsgBoxCenterCBT, NULL, GetCurrentThreadId());
    int ret = MessageBoxW(parent, text, title, type);
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    g_center_on = nullptr;
    return ret;
}

} // namespace hscrcpy
