#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include "task_store.h"
namespace tcalendar {
struct HostConfig {
    std::wstring ini_file_path;
    std::wstring default_template_path;
    std::wstring user_template_path;
    bool block_external_navigation = true;
    bool enable_webview2_bootstrap = true;
    std::wstring storage_db_path;
    bool test_force_storage_write_failure = false;
    std::wstring default_view_mode = L"list"; // "list" or "timeline"
    int default_range_preset_days = 1;        // 1/7/14/30
    int default_custom_range_days = 7;        // for custom mode
    bool default_use_custom_range = false;
    std::wstring ui_font_family = L"Segoe UI";
    int ui_base_font_size = 14;
    int ui_calendar_date_font_size = 13;
    int ui_task_font_size = 14;
    int ui_panel_right_width = 420;
    int ui_calendar_height = 420;
    bool ui_show_task_panel = true;
    // TClock-owned alert toggle storage
    std::wstring tclock_ini_file_path;
    bool tclock_alert_enabled = false;
    // Alert runtime settings
    bool enable_task_start_notify = false;
    bool alert_sound_enabled = true;
    std::wstring alert_sound_path = L"C:\\Windows\\Media\\notify.wav";
    std::wstring holiday_subscription_files;
    std::wstring holiday_subscription_catalog;
    int alert_scan_window_minutes = 60;
    int alert_dispatch_tick_seconds = 60;
    int alert_refresh_minutes = 10;
    int alert_grace_minutes = 1;
    bool startup_log_enabled = false;
    std::wstring startup_log_path;
};
bool CanonicalizeTCalendarIni(const std::wstring& ini_file_path);
class TCalendarHost {
public:
    explicit TCalendarHost(const HostConfig& config);
    ~TCalendarHost();
    bool Initialize();
    void Shutdown();
    // Phase 1: wire this to WebView2 message bridge.
    bool HandleWebMessage(const std::wstring& request_json, std::wstring& response_json);
    void SetHostWindow(HWND hwnd);
    bool IsWebView2BootstrapReady() const;
    const std::wstring& GetLastInitializeError() const;
private:
    HostConfig config_;
    bool initialized_ = false;
    TaskStore store_;
    void* webview2_loader_module_ = nullptr;
    bool webview2_bootstrap_ready_ = false;
    HWND host_window_ = nullptr;
    std::wstring last_initialize_error_;
    bool InitializeWebView2Bootstrap();
    void ShutdownWebView2Bootstrap();
    static std::wstring BuildResponse(bool ok, const wchar_t* code, const wchar_t* message,
                                      const std::wstring& request_id, const wchar_t* data_json);
};
} // namespace tcalendar
