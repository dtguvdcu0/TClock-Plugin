#pragma once
#include <limits.h>

// Preserve vertical native edges; extend horizontal edges from the adjacent OS surface.
typedef struct TBE_REGION {
 HWND window;
 RECT cut;
 SIZE size;
 BOOL applied;
 BOOL busy;
} TBE_REGION;

typedef struct TBE_FRAME {
 HDC dc;
 HBITMAP bitmap;
 HGDIOBJ previous;
 RGBQUAD* pixels;
 SIZE size;
} TBE_FRAME;

static __inline void tbe_release_frame(TBE_FRAME* frame)
{
 if (frame->dc && frame->previous) SelectObject(frame->dc, frame->previous);
 if (frame->bitmap) DeleteObject(frame->bitmap);
 if (frame->dc) DeleteDC(frame->dc);
 ZeroMemory(frame, sizeof(*frame));
}

// Keep the previous allocation intact until a replacement is selected successfully.
static __inline BOOL tbe_ensure_frame(TBE_FRAME* frame, LONG width, LONG height)
{
 BITMAPINFO info = {0};
 HDC dc;
 HBITMAP bitmap;
 HGDIOBJ previous;
 void* pixels = NULL;
 if (width <= 0 || height <= 0 || (SIZE_T)width > (SIZE_T)-1 / 4u / (SIZE_T)height) return FALSE;
 if (frame->bitmap && frame->size.cx == width && frame->size.cy == height) return TRUE;
 dc = frame->dc ? frame->dc : CreateCompatibleDC(NULL);
 if (!dc) return FALSE;
 info.bmiHeader.biSize = sizeof(info.bmiHeader);
 info.bmiHeader.biWidth = width;
 info.bmiHeader.biHeight = -height;
 info.bmiHeader.biPlanes = 1;
 info.bmiHeader.biBitCount = 32;
 info.bmiHeader.biCompression = BI_RGB;
 bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
 if (!bitmap || !pixels) {
  if (bitmap) DeleteObject(bitmap);
  if (!frame->dc) DeleteDC(dc);
  return FALSE;
 }
 previous = SelectObject(dc, bitmap);
 if (!previous || previous == HGDI_ERROR) {
  DeleteObject(bitmap);
  if (!frame->dc) DeleteDC(dc);
  return FALSE;
 }
 if (!frame->dc) frame->previous = previous;
 if (frame->bitmap) DeleteObject(frame->bitmap);
 frame->dc = dc; frame->bitmap = bitmap; frame->pixels = (RGBQUAD*)pixels;
 frame->size.cx = width; frame->size.cy = height;
 return TRUE;
}

// Copy a final source crop without changing the bitmap shared by other surfaces.
static __inline BOOL tbe_copy_frame(TBE_FRAME* frame, HDC source, const RECT* area, BOOL topDown)
{
 DIBSECTION dib = {0};
 LONG width, height, sourceHeight;
 if (GetObjectW(GetCurrentObject(source, OBJ_BITMAP), sizeof(dib), &dib) != (int)sizeof(dib) ||
     !dib.dsBm.bmBits || dib.dsBmih.biBitCount != 32 || dib.dsBmih.biHeight == LONG_MIN) return FALSE;
 sourceHeight = dib.dsBmih.biHeight < 0 ? -dib.dsBmih.biHeight : dib.dsBmih.biHeight;
 if (area->left < 0 || area->top < 0 || area->right <= area->left || area->bottom <= area->top ||
     area->right > dib.dsBm.bmWidth || area->bottom > sourceHeight) return FALSE;
 width = area->right - area->left; height = area->bottom - area->top;
 if (!tbe_ensure_frame(frame, width, height) || !GdiFlush()) return FALSE;
 for (LONG y = 0; y < height; ++y) {
  // GetObject normalizes biHeight; the caller retains the original DIB orientation.
  LONG row = topDown ? area->top + y : sourceHeight - 1 - area->top - y;
  const BYTE* pixels = (const BYTE*)dib.dsBm.bmBits + (SIZE_T)row * dib.dsBm.bmWidthBytes + (SIZE_T)area->left * 4u;
  CopyMemory(frame->pixels + (SIZE_T)y * width, pixels, (SIZE_T)width * 4u);
 }
 return TRUE;
}

static __inline void tbe_write_strip(RGBQUAD* pixels, LONG width, LONG height, const RECT* strip, COLORREF color)
{
 RECT bounds = {0, 0, width, height}, cut;
 if (!pixels || color == CLR_INVALID || !IntersectRect(&cut, &bounds, strip)) return;
 for (LONG y = cut.top; y < cut.bottom; ++y) {
  for (LONG x = cut.left; x < cut.right; ++x) {
   RGBQUAD* pixel = pixels + (SIZE_T)y * width + x;
   pixel->rgbBlue = GetBValue(color); pixel->rgbGreen = GetGValue(color);
   pixel->rgbRed = GetRValue(color); pixel->rgbReserved = 255;
  }
 }
}

static __inline BOOL tbe_get_strip(const RECT* task, const RECT* monitor, RECT* strip)
{
 LONG width = task->right - task->left, height = task->bottom - task->top;
 if (width <= 0 || height <= 0 || width == height || monitor->right <= monitor->left || monitor->bottom <= monitor->top) return FALSE;
 *strip = *task;
 if (height > width) {
  if (task->left + width / 2 < monitor->left + (monitor->right - monitor->left) / 2) strip->left = task->right - 1;
  else strip->right = task->left + 1;
 } else {
  if (task->top + height / 2 < monitor->top + (monitor->bottom - monitor->top) / 2) strip->top = task->bottom - 1;
  else strip->bottom = task->top + 1;
 }
 return TRUE;
}

static __inline HWND tbe_find_taskbar(HWND clock)
{
 HWND window = clock;
 HMONITOR monitor = MonitorFromWindow(clock, MONITOR_DEFAULTTONEAREST);
 while (window) {
  WCHAR name[80];
  if (GetClassNameW(window, name, _countof(name)) &&
      (!lstrcmpW(name, L"Shell_TrayWnd") || !lstrcmpW(name, L"Shell_SecondaryTrayWnd"))) return window;
  window = GetParent(window);
 }
 window = FindWindowW(L"Shell_TrayWnd", NULL);
 if (window && MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) == monitor) return window;
 window = NULL;
 while ((window = FindWindowExW(NULL, window, L"Shell_SecondaryTrayWnd", NULL)) != NULL)
  if (MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) == monitor) return window;
 return NULL;
}

// Only call for TClock-owned windows. Region ownership transfers to USER on success.
static __inline void tbe_update_region(HWND window, HWND taskbar, TBE_REGION* state)
{
 RECT bounds, task, strip, cut = {0, 0, 0, 0};
 MONITORINFO monitor = { sizeof(monitor) };
 SIZE size;
 HRGN region, excluded;
 BOOL hasCut = FALSE;
 if (state->busy || !window || !GetWindowRect(window, &bounds)) return;
 size.cx = bounds.right - bounds.left; size.cy = bounds.bottom - bounds.top;
 if (size.cx <= 0 || size.cy <= 0) return;
 if (taskbar && GetWindowRect(taskbar, &task) &&
     GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &monitor) &&
     task.bottom - task.top > task.right - task.left &&
     tbe_get_strip(&task, &monitor.rcMonitor, &strip) && IntersectRect(&cut, &bounds, &strip)) {
  OffsetRect(&cut, -bounds.left, -bounds.top); hasCut = TRUE;
 }
 if (state->window != window) { state->window = window; state->applied = FALSE; }
 if (!hasCut) {
  if (state->applied) {
   state->busy = TRUE;
   if (SetWindowRgn(window, NULL, TRUE)) state->applied = FALSE;
   state->busy = FALSE;
  }
  return;
 }
 if (state->applied && state->size.cx == size.cx && state->size.cy == size.cy && EqualRect(&state->cut, &cut)) return;
 region = CreateRectRgn(0, 0, size.cx, size.cy);
 excluded = CreateRectRgn(cut.left, cut.top, cut.right, cut.bottom);
 if (!region || !excluded || CombineRgn(region, region, excluded, RGN_DIFF) == ERROR) {
  if (region) DeleteObject(region);
  if (excluded) DeleteObject(excluded);
  return;
 }
 DeleteObject(excluded);
 state->busy = TRUE;
 if (SetWindowRgn(window, region, TRUE)) { state->applied = TRUE; state->cut = cut; state->size = size; }
 else DeleteObject(region);
 state->busy = FALSE;
}

// Horizontal composition has no native surface behind the clock.
// Cache only a validated native edge; transient failures retain its last good color.
typedef struct TBE_COLOR {
 HWND taskbar;
 RECT task;
 RECT monitor;
 DWORD attemptTick;
 BOOL attempted;
 BOOL fresh;
 BOOL valid;
 COLORREF color;
} TBE_COLOR;

static TBE_COLOR g_tbeColor = {0};

static __inline void tbe_reset_color(void)
{
 g_tbeColor.attempted = FALSE;
}

static __inline BOOL tbe_can_sample(HWND taskbar, POINT point)
{
 HWND window = WindowFromPoint(point);
 if (!window || GetAncestor(window, GA_ROOT) != taskbar) return FALSE;
 while (window && window != taskbar) {
  WCHAR name[80] = {0};
  if (GetClassNameW(window, name, _countof(name)) &&
      CompareStringOrdinal(name, 6, L"TClock", 6, TRUE) == CSTR_EQUAL) return FALSE;
  window = GetParent(window);
 }
 return window == taskbar;
}

static __inline COLORREF tbe_sample_color(HDC screen, HWND window, HWND taskbar, RECT* cut, TBE_COLOR* cache)
{
 RECT bounds, task, strip;
 MONITORINFO monitor = { sizeof(monitor) };
 DWORD now;
 BOOL ownDC = FALSE;
 LONG candidates[5];
 if (!taskbar || !GetWindowRect(window, &bounds) || !GetWindowRect(taskbar, &task) ||
     task.right - task.left <= task.bottom - task.top ||
     !GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &monitor) ||
     !tbe_get_strip(&task, &monitor.rcMonitor, &strip) || !IntersectRect(cut, &bounds, &strip)) return CLR_INVALID;
 OffsetRect(cut, -bounds.left, -bounds.top);
 if (cache->taskbar != taskbar || !EqualRect(&cache->task, &task) ||
     !EqualRect(&cache->monitor, &monitor.rcMonitor)) {
  ZeroMemory(cache, sizeof(*cache));
  cache->taskbar = taskbar; cache->task = task; cache->monitor = monitor.rcMonitor;
 }
 now = GetTickCount();
 if (cache->attempted && now - cache->attemptTick < (cache->fresh ? 10000u : 1000u))
  return cache->valid ? cache->color : CLR_INVALID;
 cache->attempted = TRUE; cache->fresh = FALSE; cache->attemptTick = now;
 if (!screen) { screen = GetDC(NULL); ownDC = TRUE; }
 candidates[0] = bounds.left - 4; candidates[1] = bounds.right + 4;
 candidates[2] = task.left + 4; candidates[3] = task.right - 5;
 candidates[4] = task.left + (task.right - task.left) / 2;
 if (screen) {
  for (SIZE_T i = 0; i < _countof(candidates); ++i) {
   POINT point = {candidates[i], strip.top};
   COLORREF color;
   if (point.x < task.left || point.x >= task.right || !tbe_can_sample(taskbar, point)) continue;
   color = GetPixel(screen, point.x, point.y);
   if (color != CLR_INVALID) { cache->color = color; cache->valid = TRUE; cache->fresh = TRUE; break; }
  }
 }
 if (ownDC && screen) ReleaseDC(NULL, screen);
 return cache->valid ? cache->color : CLR_INVALID;
}

// Main and notification surfaces keep their existing taskbar-local cache.
static __inline COLORREF tbe_sample_strip(HDC screen, HWND window, HWND taskbar, RECT* cut)
{
 return tbe_sample_color(screen, window, taskbar, cut, &g_tbeColor);
}

// Attached overlays leave the native/clock border visible; floating clocks do not call this.
static __inline void tbe_clear_strip(RGBQUAD* pixels, LONG width, LONG height,
 const RECT* bounds, HWND taskbar, BOOL topDown)
{
 RECT task, strip, cut, bitmap = {0, 0, width, height};
 MONITORINFO monitor = { sizeof(monitor) };
 if (!pixels || !taskbar || !GetWindowRect(taskbar, &task) ||
     !GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &monitor) ||
     !tbe_get_strip(&task, &monitor.rcMonitor, &strip) || !IntersectRect(&cut, bounds, &strip)) return;
 OffsetRect(&cut, -bounds->left, -bounds->top);
 if (!IntersectRect(&cut, &cut, &bitmap)) return;
 for (LONG y = cut.top; y < cut.bottom; ++y) {
  LONG row = topDown ? y : height - 1 - y;
  ZeroMemory(pixels + (SIZE_T)row * width + cut.left, (SIZE_T)(cut.right - cut.left) * sizeof(*pixels));
 }
}

// Offscreen-only helper: visible surfaces must present a completed frame instead.
static __inline void tbe_draw_strip(HDC target, HWND window, HWND taskbar)
{
 RECT cut;
 COLORREF color = tbe_sample_strip(NULL, window, taskbar, &cut);
 if (color != CLR_INVALID) {
  COLORREF previous = SetDCBrushColor(target, color);
  FillRect(target, &cut, (HBRUSH)GetStockObject(DC_BRUSH));
  SetDCBrushColor(target, previous);
 }
}
