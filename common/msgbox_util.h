#pragma once

#include <windows.h>

// Shared MessageBox centering utility.
// All MessageBox calls across the app should route through these functions
// so that centering is consistent and the CBT hook is managed in one place.
namespace hscrcpy {

// Show a modal MessageBox centered on center_on (defaults to parent if NULL).
// parent    = modal owner (parent window is disabled while open)
// center_on = visual centering target (where the box appears)
int centered_msgbox_a(HWND parent, const char *text, const char *title, UINT type,
                      HWND center_on = nullptr);
int centered_msgbox_w(HWND parent, const wchar_t *text, const wchar_t *title, UINT type,
                      HWND center_on = nullptr);

} // namespace hscrcpy
