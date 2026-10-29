#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fstream>
#include <filesystem>
#include <cstdint>

LRESULT CALLBACK fixture_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if(message==WM_TIMER) {DestroyWindow(hwnd);return 0;}
  if (message == WM_PAINT) {
    PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); RECT rect{}; GetClientRect(hwnd, &rect);
    HBRUSH brush = CreateSolidBrush(RGB(42, 180, 95)); FillRect(dc, &rect, brush); DeleteObject(brush);
    rect.right = rect.left + (rect.right - rect.left) / 2;
    brush = CreateSolidBrush(RGB(210, 35, 75)); FillRect(dc, &rect, brush); DeleteObject(brush);
    EndPaint(hwnd, &ps); return 0;
  }
  if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(hwnd, message, wp, lp);
}
int wmain(int argc, wchar_t** argv) {
  if (argc != 2 && argc != 3) return 2;
  WNDCLASSW wc{}; wc.lpfnWndProc = fixture_proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"OpenXMBSessionWindowFixture"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  if (!RegisterClassW(&wc)) return 3;
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"OpenXMB capture fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 480, 320, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) return 4;
  UpdateWindow(hwnd);
  if(argc==3) SetTimer(hwnd,1,static_cast<UINT>(std::max(1,_wtoi(argv[2]))),nullptr);
  { std::ofstream out{std::filesystem::path(argv[1])}; out << reinterpret_cast<std::uintptr_t>(hwnd); }
  MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  return 0;
}
#else
int main() { return 0; }
#endif

