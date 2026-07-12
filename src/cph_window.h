#ifndef HSCRCPY_CPH_WINDOW_H
#define HSCRCPY_CPH_WINDOW_H

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>

namespace hscrcpy {

// Haps permission entry: name + granted flag (parsed from bm dump-permissions)
struct HapsPermission {
    std::string name;
    bool granted;
    bool system_fixed;
};

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
    static void reset();
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
        // Haps tab
        IDC_HAPS_PKG_LIST,
        IDC_HAPS_PERM_LIST,
        IDC_HAPS_REFRESH_BTN,
        IDC_HAPS_GRANT_ALL_BTN,
        IDC_HAPS_GRANT_BTN,
        IDC_HAPS_REVOKE_BTN,
        IDC_HAPS_REVOKE_ALL_BTN,
        IDC_HAPS_COPY_PERM_BTN,
        IDC_HAPS_COPY_PKG_BTN,
    };

    // Tab page indices
    enum TabPage : int {
        TAB_GNSS = 0,
        TAB_BATTERY = 1,
        TAB_HAPS = 2,
        TAB_COUNT = 3,
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
        ASYNC_HAPS_LIST_PACKAGES,
        ASYNC_HAPS_LIST_PERMISSIONS,
        ASYNC_HAPS_GRANT,
        ASYNC_HAPS_REVOKE,
        ASYNC_HAPS_GRANT_ALL,
        ASYNC_HAPS_REVOKE_ALL,
    };
    struct AsyncResult {
        AsyncOp op;
        bool ok;
        std::string output;
        std::string cmd;
    };
    static void run_async(AsyncOp op, const std::string &cmd = "");
    static void handle_async_done(AsyncResult *r);
    static std::atomic<bool>* busy_flag_for(AsyncOp op);

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

    // Haps - Package & permission management via bm command
    static void haps_list_packages();
    static void haps_list_permissions();
    static void haps_grant(const std::string &perms_csv);
    static void haps_revoke(const std::string &perms_csv);
    static void haps_grant_all();
    static void haps_revoke_all();
    static void haps_populate_packages();
    static void haps_populate_permissions(const std::vector<HapsPermission> &perms);
    static std::string haps_get_selected_perms_csv();

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

    // Haps - Package & permission management
    static HWND g_haps_pkg_list;
    static HWND g_haps_perm_list;
    static HWND g_haps_refresh_btn;
    static HWND g_haps_grant_all_btn;
    static HWND g_haps_grant_btn;
    static HWND g_haps_revoke_btn;
    static HWND g_haps_revoke_all_btn;
    static std::vector<std::string> g_haps_packages;
    static std::string g_haps_selected_pkg;
    static std::string g_haps_user_id;
    static bool g_haps_loaded;
    static bool g_bat_inject_filled;

    // Tab page containers
    static HWND g_tab_pages[TAB_COUNT];

    static OnShellCommand g_on_shell;
    static OnPushFile g_on_push;
    static OnBusy g_on_busy;
    static int g_current_tab;
    static std::atomic<bool> g_gnss_busy;
    static std::atomic<bool> g_battery_busy;
    static std::atomic<bool> g_haps_busy;
    static UINT_PTR g_refresh_timer;

    static HWND g_toast_hwnd;
};

} // namespace hscrcpy

#endif
