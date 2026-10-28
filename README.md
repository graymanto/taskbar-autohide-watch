# Taskbar Auto-Hide Watch

A small Win32, per-user background utility that repairs auto-hide after display changes, system resume and session unlock. If you are irritated by windows resetting the auto-hide state of the taskbar when a computer wakes from sleep, this utility is for you.

## Build (Visual Studio Developer PowerShell / Developer Command Prompt)

```powershell
cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE taskbar-autohide-watch.cpp /link /SUBSYSTEM:WINDOWS /OUT:taskbar-autohide-watch.exe
```

## Run

Run `taskbar-autohide-watch.exe`. It has no console/window/tray icon. One instance per interactive user session is allowed. Log: `%LOCALAPPDATA%\TaskbarAutoHideWatch\taskbar-watch.log`.

**Manual repair:** Ctrl+Alt+Shift+T.

**Stop:** Task Manager -> Details -> taskbar-autohide-watch.exe -> End task; or `taskkill /IM taskbar-autohide-watch.exe /F` (will terminate other sessions' instances if accessible).

**Start at login:** create a shortcut pointing to the exe in `Win+R` -> `shell:startup`. Store the exe in a stable path (e.g. `%LOCALAPPDATA%\Programs\TaskbarAutoHideWatch`).

## Behavior

- On display change, resume or session unlock, wait 5 seconds after the last event. Check the Shell API; restore auto-hide only if it is off.
- Retry an unsuccessful repair up to six checks during a 60-second recovery window.
- Poll the active monitor count every two seconds as fallback for display topology changes without a broadcast.
- Observe live state once per second, but **do not** repair outside recovery windows.

Limitation: `WM_DISPLAYCHANGE` isn't guaranteed for every topology change; a same-count monitor swap may be missed. `ABM_GETSTATE` is global taskbar state, not independent per-monitor taskbar properties. A manual off-toggle during an active recovery window can be overridden; outside the window it is respected.
