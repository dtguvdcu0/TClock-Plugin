#pragma once

#include <windows.h>

// Shared visibility contract for layered surfaces belonging to the native taskbar.
static __inline BOOL tbs_is_uncovered(HWND foreground, HWND taskbar,
 const RECT* windowRect, const RECT* displayRect)
{
 DWORD foregroundPid = 0, taskbarPid = 0;
 WCHAR className[64];
 RECT local, current;
 HRGN region;
 int kind;
 BOOL uncovered = FALSE;
 if (!displayRect || IsRectEmpty(displayRect) ||
     (GetWindowLongPtrW(foreground, GWL_EXSTYLE) & WS_EX_LAYOUTRTL)) return FALSE;
 GetWindowThreadProcessId(foreground, &foregroundPid);
 GetWindowThreadProcessId(taskbar, &taskbarPid);
 if (!taskbarPid || foregroundPid != taskbarPid ||
     GetClassNameW(foreground, className, _countof(className)) <= 0 ||
     lstrcmpW(className, L"XamlExplorerHostIslandWindow") != 0) return FALSE;
 region = CreateRectRgn(0, 0, 0, 0);
 if (!region) return FALSE;
 kind = GetWindowRgn(foreground, region);
 if (kind == SIMPLEREGION || kind == COMPLEXREGION) {
  // The drawing region uses window coordinates, including on negative-origin displays.
  local = *displayRect;
  OffsetRect(&local, -windowRect->left, -windowRect->top);
  uncovered = !RectInRegion(region, &local) &&
   GetAncestor(GetForegroundWindow(), GA_ROOTOWNER) == foreground &&
   IsWindowVisible(foreground) && !IsIconic(foreground) &&
   GetWindowRect(foreground, &current) && EqualRect(windowRect, &current);
 }
 DeleteObject(region);
 return uncovered;
}

static __inline BOOL tbs_can_present_rect(HWND target, HWND taskbar, const RECT* displayRect)
{
 HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOTOWNER);
 RECT rect, client;
 MONITORINFO monitor = { sizeof(monitor) };
 WCHAR className[64];
 if (!IsWindowVisible(taskbar) || !IsWindowVisible(target) ||
     !GetClientRect(target, &client) || client.right <= 0 || client.bottom <= 0) return FALSE;
 if (!foreground || foreground == GetShellWindow() || foreground == taskbar ||
     IsIconic(foreground) || !IsWindowVisible(foreground)) return TRUE;
 className[0] = L'\0';
 if (GetClassNameW(foreground, className, _countof(className)) > 0 &&
     (lstrcmpW(className, L"Progman") == 0 || lstrcmpW(className, L"WorkerW") == 0)) return TRUE;
 if (GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &monitor) &&
     GetWindowRect(foreground, &rect) && rect.left <= monitor.rcMonitor.left &&
     rect.top <= monitor.rcMonitor.top && rect.right >= monitor.rcMonitor.right &&
     rect.bottom >= monitor.rcMonitor.bottom) {
  RECT bounds;
  if (displayRect) bounds = *displayRect;
  else if (!GetWindowRect(target, &bounds)) return FALSE;
  return tbs_is_uncovered(foreground, taskbar, &rect, &bounds);
 }
 return TRUE;
}

static __inline BOOL tbs_can_present(HWND target, HWND taskbar)
{
 return tbs_can_present_rect(target, taskbar, NULL);
}

// Preserve the shell's stacking instead of giving each surface an independent topmost rank.
static __inline BOOL tbs_sync_order(HWND surface, HWND taskbar)
{
 HWND preceding = GetWindow(taskbar, GW_HWNDPREV);
 if (preceding == surface) preceding = GetWindow(surface, GW_HWNDPREV);
 return SetWindowPos(surface, preceding ? preceding : HWND_TOP, 0, 0, 0, 0,
  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}
