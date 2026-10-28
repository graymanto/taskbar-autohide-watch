#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace {
constexpr UINT_PTR kHeartbeatTimer = 1;
constexpr int kHotkeyId = 1;
constexpr DWORD kHotkeyModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
constexpr ULONGLONG kSettleMs = 5000;
constexpr ULONGLONG kRecoveryWindowMs = 60000;
constexpr ULONGLONG kRetryMs = 5000;
constexpr ULONGLONG kMonitorPollMs = 2000;
constexpr int kMaxAttempts = 6;
constexpr wchar_t kWindowClass[] = L"TaskbarAutoHideWatchWindowV1";
constexpr wchar_t kMutexName[] = L"Local\\TaskbarAutoHideWatchV1";

FILE* g_log = nullptr;
HANDLE g_mutex = nullptr;
int g_monitorCount = -1;
ULONGLONG g_lastMonitorPoll = 0;
ULONGLONG g_recoveryEnd = 0;
ULONGLONG g_nextAttempt = 0;
int g_attempts = 0;
bool g_recoveryPending = false;
bool g_lastStateKnown = false;
bool g_lastAutoHide = false;

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fwprintf(g_log, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
        st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_log, fmt, args);
    va_end(args);
    fputwc(L'\n', g_log);
    fflush(g_log);
}

void OpenLog() {
    wchar_t base[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    std::wstring directory = std::wstring(base) + L"\\TaskbarAutoHideWatch";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) return;
    std::wstring path = directory + L"\\taskbar-watch.log";
    _wfopen_s(&g_log, path.c_str(), L"a, ccs=UTF-8");
}

UINT TaskbarState(HWND hwnd) {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    return static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &data));
}

void ObserveTaskbar(HWND hwnd) {
    UINT state = TaskbarState(hwnd);
    bool autoHide = (state & ABS_AUTOHIDE) != 0;
    if (!g_lastStateKnown || autoHide != g_lastAutoHide) {
        Log(L"Taskbar auto-hide changed: %s (state=0x%X)",
            autoHide ? L"ON" : L"OFF", state);
        g_lastAutoHide = autoHide;
        g_lastStateKnown = true;
    }
}

bool RepairTaskbar(HWND hwnd) {
    UINT state = TaskbarState(hwnd);
    if (state & ABS_AUTOHIDE) return true;

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.lParam = static_cast<LPARAM>(state | ABS_AUTOHIDE);
    Log(L"Auto-hide OFF: calling ABM_SETSTATE (desired=0x%X)",
        static_cast<unsigned int>(data.lParam));
    SHAppBarMessage(ABM_SETSTATE, &data);

    UINT result = TaskbarState(hwnd);
    bool success = (result & ABS_AUTOHIDE) != 0;
    Log(L"Repair verification: %s (state=0x%X)",
        success ? L"ON" : L"OFF", result);
    ObserveTaskbar(hwnd);
    return success;
}

void ScheduleRecovery(const wchar_t* reason) {
    const ULONGLONG now = GetTickCount64();
    g_recoveryPending = true;
    g_recoveryEnd = now + kRecoveryWindowMs;
    g_nextAttempt = now + kSettleMs;
    g_attempts = 0;
    Log(L"Recovery scheduled (%s); checking in %llu ms", reason, kSettleMs);
}

void Tick(HWND hwnd) {
    ULONGLONG now = GetTickCount64();
    ObserveTaskbar(hwnd);  // Observe only; never undo an ordinary manual change.

    if (now - g_lastMonitorPoll >= kMonitorPollMs) {
        g_lastMonitorPoll = now;
        int count = GetSystemMetrics(SM_CMONITORS);
        if (count != g_monitorCount) {
            Log(L"Monitor count: %d -> %d", g_monitorCount, count);
            g_monitorCount = count;
            ScheduleRecovery(L"monitor count changed");
        }
    }

    if (!g_recoveryPending || now < g_nextAttempt) return;
    if (now >= g_recoveryEnd || g_attempts >= kMaxAttempts) {
        Log(L"Recovery window ended without verified repair");
        g_recoveryPending = false;
        return;
    }

    ++g_attempts;
    Log(L"Recovery check %d/%d (monitors=%d)",
        g_attempts, kMaxAttempts, GetSystemMetrics(SM_CMONITORS));
    if (RepairTaskbar(hwnd)) {
        Log(L"Recovery completed (auto-hide enabled)");
        g_recoveryPending = false;
    } else {
        g_nextAttempt = now + kRetryMs;
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        g_monitorCount = GetSystemMetrics(SM_CMONITORS);
        g_lastMonitorPoll = GetTickCount64();
        Log(L"Started; monitors=%d; diagnostic auto-hide check only on startup",
            g_monitorCount);
        ObserveTaskbar(hwnd);
        if (!SetTimer(hwnd, kHeartbeatTimer, 1000, nullptr)) {
            Log(L"SetTimer failed: %lu", GetLastError());
            return -1;
        }
        if (!RegisterHotKey(hwnd, kHotkeyId, kHotkeyModifiers, 'T'))
            Log(L"Warning: Ctrl+Alt+Shift+T could not be registered: %lu", GetLastError());
        if (!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION))
            Log(L"Warning: session notifications unavailable: %lu", GetLastError());
        return 0;

    case WM_DISPLAYCHANGE:
        Log(L"WM_DISPLAYCHANGE; monitors=%d", GetSystemMetrics(SM_CMONITORS));
        g_monitorCount = GetSystemMetrics(SM_CMONITORS);
        ScheduleRecovery(L"WM_DISPLAYCHANGE");
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            Log(L"Resume event: 0x%llX", static_cast<unsigned long long>(wp));
            ScheduleRecovery(L"resume");
        }
        return TRUE;

    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT ||
            wp == WTS_REMOTE_CONNECT) {
            Log(L"Session event: 0x%llX", static_cast<unsigned long long>(wp));
            ScheduleRecovery(L"session unlock/connect");
        }
        return 0;

    case WM_SETTINGCHANGE:
        // Important: do not schedule a repair here. A user may have
        // intentionally disabled auto-hide via Windows Settings.
        ObserveTaskbar(hwnd);
        return 0;

    case WM_HOTKEY:
        if (wp == kHotkeyId) {
            Log(L"Manual repair hotkey pressed");
            RepairTaskbar(hwnd);
        }
        return 0;

    case WM_TIMER:
        if (wp == kHeartbeatTimer) Tick(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kHeartbeatTimer);
        UnregisterHotKey(hwnd, kHotkeyId);
        WTSUnRegisterSessionNotification(hwnd);
        Log(L"Stopped");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!g_mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        return 0;
    }

    OpenLog();
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassW(&wc)) {
        Log(L"RegisterClass failed: %lu", GetLastError());
        if (g_log) fclose(g_log);
        CloseHandle(g_mutex);
        return 1;
    }

    // Unshown TOP-LEVEL window receives display/power broadcasts.
    // A message-only window (HWND_MESSAGE) would not receive them.
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"Taskbar Auto-Hide Watch",
        WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        Log(L"CreateWindowEx failed: %lu", GetLastError());
        if (g_log) fclose(g_log);
        CloseHandle(g_mutex);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (g_log) fclose(g_log);
    CloseHandle(g_mutex);
    return static_cast<int>(msg.wParam);
}
