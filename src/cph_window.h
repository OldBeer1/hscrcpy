#ifndef HSCRCPY_CPH_WINDOW_H
#define HSCRCPY_CPH_WINDOW_H

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace hscrcpy {

// CPH (Cloud Phone Host) simulation window
// Provides device simulation injection: GNSS, Battery, and future expansions
// Cloud phone detection: "cmd" command exists in hdc shell
class CphWindow {
public:
    using OnShellCommand = std::function<bool(const std::string &cmd, std::string *output, int timeout_sec)>;
    using OnPushFile = std::function<bool(const std::string &local_path, const std::string &remote_path)>;
    using OnBusy = std::function<void(bool busy)>;

    static void create(HINSTANCE inst, HWND parent, int width = 440);
    static void show(OnShellCommand on_shell, OnPushFile on_push, OnBusy on_busy);
    static void hide();
    static void destroy();
    static bool is_visible();
    static HWND hwnd();
    static int panel_width();
    static void set_panel_width(int w);
    static void reposition(int x, int y, int w, int h);

    // Check if the connected device is a cloud phone (has "cmd" command)
    static bool is_cloud_phone(OnShellCommand on_shell = nullptr);

private:
    static constexpr int HEADER_H = 32;
    static constexpr int MARGIN = 8;
    static constexpr int LABEL_W = 70;
    static constexpr int EDIT_H = 24;
    static constexpr int BTN_H = 28;
    static constexpr int ROW_GAP = 4;

    // Control IDs
    enum CtrlId : int {
        IDC_TAB = 3001,
        // GNSS - Location inject
        IDC_GNSS_LAT_EDIT,
        IDC_GNSS_LNG_EDIT,
        IDC_GNSS_ALT_EDIT,
        IDC_GNSS_SPEED_EDIT,
        IDC_GNSS_BEARING_EDIT,
        IDC_GNSS_HACC_EDIT,
        IDC_GNSS_VACC_EDIT,
        IDC_GNSS_SACC_EDIT,
        IDC_GNSS_BACC_EDIT,
        IDC_GNSS_INJECT_BTN,
        IDC_GNSS_REFRESH_BTN,
        // GNSS - Current location display
        IDC_GNSS_CURRENT_LABEL,
        // GNSS - Trajectory
        IDC_GNSS_TRAJ_EDIT,
        IDC_GNSS_TRAJ_BROWSE_BTN,
        IDC_GNSS_TRAJ_START_BTN,
        IDC_GNSS_TRAJ_STOP_BTN,
        // Battery tab
        IDC_BAT_CAPACITY_EDIT,
        IDC_BAT_TEMP_EDIT,
        IDC_BAT_VOLTAGE_EDIT,
        IDC_BAT_VOLTAGE_MAX_EDIT,
        IDC_BAT_CURRENT_EDIT,
        IDC_BAT_CURRENT_AVG_EDIT,
        IDC_BAT_CURRENT_MAX_EDIT,
        IDC_BAT_HEALTH_COMBO,
        IDC_BAT_PRESENT_COMBO,
        IDC_BAT_TOTAL_ENERGY_EDIT,
        IDC_BAT_TECHNOLOGY_EDIT,
        IDC_BAT_PLUGGED_COMBO,
        IDC_BAT_CHARGER_COMBO,
        IDC_BAT_CHARGESTATE_COMBO,
        IDC_BAT_INJECT_BTN,
        IDC_BAT_RESET_BTN,
        IDC_BAT_REFRESH_BTN,
        IDC_BAT_CURRENT_LABEL,
        IDC_BAT_SCENE_COMBO,
        IDC_BAT_SCENE_BTN,
        // Status
        IDC_STATUS_LABEL,
    };

    // Tab page indices
    enum TabPage : int {
        TAB_GNSS = 0,
        TAB_BATTERY = 1,
        // Future tabs: TAB_SENSOR, TAB_CAMERA, TAB_AUDIO, etc.
        TAB_COUNT = 2,
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    static void on_tab_change();
    static void show_tab_page(int page);

    // Async shell task runner: runs shell on background thread, posts result to UI
    enum AsyncOp : int {
        ASYNC_REFRESH_GNSS = 1,
        ASYNC_INJECT_GNSS,
        ASYNC_START_TRAJ,
        ASYNC_STOP_TRAJ,
        ASYNC_REFRESH_BATTERY,
        ASYNC_INJECT_BATTERY,
        ASYNC_RESET_BATTERY,
        ASYNC_INJECT_SCENE,
    };
    struct AsyncResult {
        AsyncOp op;
        bool ok;
        std::string output;
        std::string cmd;
    };
    static void run_async(AsyncOp op, const std::string &cmd = "");
    static void handle_async_done(AsyncResult *r);

    // GNSS
    static void inject_gnss();
    static void refresh_gnss();
    static void refresh_gnss_fill_edits();
    static void start_trajectory();
    static void stop_trajectory();
    static void parse_gnss_location(const std::string &output, bool fill_edits = false);

    // Battery
    static void inject_battery();
    static void reset_battery();
    static void refresh_battery();
    static void inject_charge_scene();
    static void parse_battery_info(const std::string &output, bool fill_edits = false);

    static void msg_box(const char *title, const char *fmt, ...);
    static void show_toast(const char *text, bool success = true);
    static void set_status(const char *text);

    static HWND g_hwnd;
    static HINSTANCE g_inst;
    static HWND g_parent;
    static int g_panel_w;

    // Tab control
    static HWND g_tab;

    // GNSS - Location inject controls
    static HWND g_gnss_lat_edit;
    static HWND g_gnss_lng_edit;
    static HWND g_gnss_alt_edit;
    static HWND g_gnss_speed_edit;
    static HWND g_gnss_bearing_edit;
    static HWND g_gnss_hacc_edit;
    static HWND g_gnss_vacc_edit;
    static HWND g_gnss_sacc_edit;
    static HWND g_gnss_bacc_edit;
    static HWND g_gnss_inject_btn;
    static HWND g_gnss_refresh_btn;
    static HWND g_gnss_current_label;

    // GNSS - Trajectory controls
    static HWND g_gnss_traj_edit;
    static HWND g_gnss_traj_browse_btn;
    static HWND g_gnss_traj_start_btn;
    static HWND g_gnss_traj_stop_btn;

    // Battery controls
    static HWND g_bat_capacity_edit;
    static HWND g_bat_temp_edit;
    static HWND g_bat_voltage_edit;
    static HWND g_bat_voltage_max_edit;
    static HWND g_bat_current_edit;
    static HWND g_bat_current_avg_edit;
    static HWND g_bat_current_max_edit;
    static HWND g_bat_health_combo;
    static HWND g_bat_present_combo;
    static HWND g_bat_total_energy_edit;
    static HWND g_bat_technology_edit;
    static HWND g_bat_plugged_combo;
    static HWND g_bat_charger_combo;
    static HWND g_bat_chargestate_combo;
    static HWND g_bat_inject_btn;
    static HWND g_bat_reset_btn;
    static HWND g_bat_refresh_btn;
    static HWND g_bat_current_label;
    static HWND g_bat_scene_combo;
    static HWND g_bat_scene_btn;

    // Status
    static HWND g_status_label;

    // Tab page containers
    static HWND g_tab_pages[TAB_COUNT];

    static OnShellCommand g_on_shell;
    static OnPushFile g_on_push;
    static OnBusy g_on_busy;
    static int g_current_tab;
    static std::atomic<bool> g_async_busy;
    static UINT_PTR g_refresh_timer;

    static HWND g_toast_hwnd;
};

} // namespace hscrcpy

#endif
