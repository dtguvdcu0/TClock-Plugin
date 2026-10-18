#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <objbase.h>
#include <fstream>
#include <algorithm>
#include <thread>
#include "settings.h"
#include "open_app.h"

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
namespace fs = std::filesystem;

struct DisplayInfo {
    std::string name;
    RECT rect;
    int index;
    HMONITOR monitor = nullptr;
};

std::wstring makeCollisionFilename(const std::wstring& filename, unsigned int sequence) {
    if (sequence <= 1) return filename;
    fs::path path(filename);
    return (path.parent_path() / (path.stem().wstring() + L"_" + std::to_wstring(sequence) + path.extension().wstring())).wstring();
}

bool saveImageWic(const std::wstring& filename, int width, int height,
                  std::vector<uint8_t>& bgr, int stride,
                  const std::string& format, int compression, std::wstring& savedFilename) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    IStream* stream = nullptr;

    auto cleanup = [&]() {
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        if (props) props->Release();
        if (factory) factory->Release();
    };

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::cerr << "Failed to create WIC factory. HRESULT: " << std::hex << hr << std::endl;
        cleanup();
        return false;
    }

    std::wstring candidate;
    for (unsigned int sequence = 1; sequence <= 10000; ++sequence) {
        candidate = makeCollisionFilename(filename, sequence);
        hr = SHCreateStreamOnFileEx(candidate.c_str(),
                                    STGM_WRITE | STGM_SHARE_DENY_WRITE | STGM_FAILIFTHERE,
                                    FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream);
        if (SUCCEEDED(hr)) break;
        if (hr != HRESULT_FROM_WIN32(ERROR_FILE_EXISTS)) {
            std::cerr << "Failed to reserve output file. HRESULT: " << std::hex << hr << std::endl;
            cleanup();
            return false;
        }
    }
    if (!stream) {
        std::cerr << "Could not reserve a unique output filename." << std::endl;
        cleanup();
        return false;
    }

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
    GUID container = GUID_ContainerFormatPng;
    std::string fmtLower = toLower(format);
    if (fmtLower == "jpg") {
        container = GUID_ContainerFormatJpeg;
    }

    hr = factory->CreateEncoder(container, nullptr, &encoder);
    if (FAILED(hr)) {
        std::cerr << "Failed to create encoder. HRESULT: " << std::hex << hr << std::endl;
        cleanup();
        return false;
    }

    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize encoder. HRESULT: " << std::hex << hr << std::endl;
        cleanup();
        return false;
    }

    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) {
        std::cerr << "Failed to create frame. HRESULT: " << std::hex << hr << std::endl;
        cleanup();
        return false;
    }

    if (props) {
        // Apply compression/quality if supported by the encoder.
        if (container == GUID_ContainerFormatPng) {
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"CompressionLevel");
            VARIANT val;
            VariantInit(&val);
            val.vt = VT_UI1;
            val.bVal = static_cast<UCHAR>(std::clamp(compression, 0, 9));
            props->Write(1, &option, &val);
            VariantClear(&val);

            // Use adaptive filtering to help reduce size while staying lossless.
            option = {};
            option.pstrName = const_cast<LPOLESTR>(L"FilterOption");
            VariantInit(&val);
            val.vt = VT_UI1;
            val.bVal = static_cast<UCHAR>(WICPngFilterAdaptive);
            props->Write(1, &option, &val);
            VariantClear(&val);
        } else if (container == GUID_ContainerFormatJpeg) {
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT val;
            VariantInit(&val);
            val.vt = VT_R4;
            float quality = std::clamp(compression, 1, 100) / 100.0f;
            val.fltVal = quality;
            props->Write(1, &option, &val);
            VariantClear(&val);
        }
    }

    hr = frame->Initialize(props);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize frame. HRESULT: " << std::hex << hr << std::endl;
        cleanup();
        return false;
    }

    hr = frame->SetSize(width, height);
    if (FAILED(hr)) {
        std::cerr << "Failed to set frame size. HRESULT: " << std::hex << hr << std::endl;
        cleanup();
        return false;
    }

    hr = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(hr)) {
        std::cerr << "Failed to set pixel format. HRESULT: " << std::hex << hr << std::endl;
        cleanup();
        return false;
    }

    hr = frame->WritePixels(static_cast<UINT>(height),
                            static_cast<UINT>(stride),
                            static_cast<UINT>(bgr.size()),
                            reinterpret_cast<BYTE*>(bgr.data()));
    if (FAILED(hr)) {
        std::cerr << "Failed to write pixels. HRESULT: " << std::hex << hr << std::endl;
        cleanup();
        return false;
    }

    hr = frame->Commit();
    if (FAILED(hr)) {
        std::cerr << "Failed to commit frame. HRESULT: " << std::hex << hr << std::endl;
        cleanup();
        return false;
    }

    hr = encoder->Commit();
    if (FAILED(hr)) {
        std::cerr << "Failed to commit encoder. HRESULT: " << std::hex << hr << std::endl;
        cleanup();
        return false;
    }

    savedFilename = candidate;
    cleanup();
    return true;
}

class ScreenshotTool {
private:
    std::vector<DisplayInfo> displays;
    std::vector<std::wstring> openedImages;

public:
    ScreenshotTool() = default;

    bool detectDisplays() {
        displays.clear();

        auto enumProc = [](HMONITOR hMon, HDC, LPRECT, LPARAM lpData) -> BOOL {
            auto* list = reinterpret_cast<std::vector<DisplayInfo>*>(lpData);
            MONITORINFOEX info{};
            info.cbSize = sizeof(MONITORINFOEX);

            if (!GetMonitorInfo(hMon, &info)) {
                return TRUE;
            }

            DisplayInfo d{};
            d.name = info.szDevice;
            d.rect = info.rcMonitor;
            d.index = static_cast<int>(list->size());
            d.monitor = hMon;
            list->push_back(d);
            return TRUE;
        };

        if (!EnumDisplayMonitors(nullptr, nullptr, enumProc, reinterpret_cast<LPARAM>(&displays))) {
            std::cerr << "Failed to enumerate monitors." << std::endl;
            return false;
        }

        return !displays.empty();
    }

    bool openImages(const AppSettings& settings) {
        bool success = true;
        for (const auto& filename : openedImages) {
            const HRESULT result = tcap_open_image(filename, settings.openApp, settings.openExecutable);
            if (FAILED(result)) {
                success = false;
                const std::wstring message = (settings.language == "ja" ?
                    L"\u753b\u50cf\u3092\u30a2\u30d7\u30ea\u3067\u958b\u3051\u307e\u305b\u3093\u3067\u3057\u305f\u3002\u958b\u304f\u30a2\u30d7\u30ea\u3092\u9078\u3073\u76f4\u3057\u3066\u304f\u3060\u3055\u3044\u3002\n\u753b\u50cf\u306f\u6b21\u306e\u5834\u6240\u306b\u6b8b\u3057\u3066\u3044\u307e\u3059:\n" :
                    L"Could not open the image. Select another app in settings.\nThe image was kept at:\n") + filename;
                MessageBoxW(nullptr, message.c_str(), L"TCapture", MB_OK | MB_ICONERROR);
            }
        }
        return success;
    }

    void listDisplays() const {
        std::cout << "Detected displays: " << displays.size() << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        for (const auto& d : displays) {
            int displayNumber = d.index + 1; // 1-based for user
            int width = d.rect.right - d.rect.left;
            int height = d.rect.bottom - d.rect.top;
            std::cout << "[" << displayNumber << "] " << d.name << std::endl;
            std::cout << "    Resolution: " << width << "x" << height << std::endl;
            std::cout << "    Position: (" << d.rect.left << ", " << d.rect.top << ")" << std::endl;
        }
    }

    int findDisplayIndex(HMONITOR mon) const {
        if (!mon) return -1;
        for (const auto& d : displays) {
            if (d.monitor == mon) return d.index;
        }
        return -1;
    }

    std::wstring generateFilename(const std::string& displayLabel, const std::wstring& outputDir,
                                  const std::string& format, std::time_t timestamp,
                                  int frameNumber, bool includeFrame) const {
        auto tm = *std::localtime(&timestamp);

        std::ostringstream oss;
        std::string ext = format == "jpg" ? "jpg" : "png";
        oss << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_" << displayLabel;
        if (includeFrame) {
            int frame = frameNumber <= 0 ? 1 : frameNumber;
            oss << "_" << frame << "f";
        }
        oss << "." << ext;

        std::wstring outputPath = outputDir.empty() ? L"." : outputDir;
        if (outputPath.empty()) return {};
        fs::path path(outputPath);
        path /= utf8ToWide(oss.str());
        return path.wstring();
    }

    bool captureDisplay(int displayIndex, const AppSettings& settings, std::time_t timestamp, int frameNumber, bool includeFrame) {
        if (displayIndex < 0 || displayIndex >= static_cast<int>(displays.size())) {
            std::cerr << "Invalid display index: " << displayIndex << std::endl;
            return false;
        }

        const auto& d = displays[displayIndex];
        std::string label = std::to_string(displayIndex + 1);
        return captureRegion(d.rect, displayIndex, label, settings, timestamp, frameNumber, includeFrame);
    }

    bool captureActiveWindow(const AppSettings& settings, std::time_t timestamp, int frameNumber, bool includeFrame) {
        HWND fg = GetForegroundWindow();
        if (!fg) {
            std::cerr << "No foreground window found." << std::endl;
            return false;
        }
        RECT rc{};
        bool haveRect = false;
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            using DwmGetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
            auto fn = reinterpret_cast<DwmGetWindowAttributeFn>(GetProcAddress(dwm, "DwmGetWindowAttribute"));
            if (fn) {
                RECT frame{};
                if (SUCCEEDED(fn(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
                    rc = frame;
                    haveRect = true;
                }
            }
            FreeLibrary(dwm);
        }
        if (!haveRect) {
            if (!GetWindowRect(fg, &rc)) {
                std::cerr << "Failed to get foreground window rect." << std::endl;
                return false;
            }
        }
        HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        int idx = findDisplayIndex(mon);
        if (idx < 0 && !displays.empty()) idx = 0;
        return captureRegion(rc, idx, "w", settings, timestamp, frameNumber, includeFrame);
    }

    bool captureActiveDisplay(const AppSettings& settings, std::time_t timestamp, int frameNumber, bool includeFrame) {
        HMONITOR mon = nullptr;
        HWND fg = GetForegroundWindow();
        if (fg) {
            mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        }
        if (!mon) {
            POINT pt{};
            if (GetCursorPos(&pt)) {
                mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            }
        }
        int idx = findDisplayIndex(mon);
        if (idx < 0) {
            std::cerr << "Active display not found; defaulting to display 1." << std::endl;
            idx = 0;
        }
        RECT rc = displays[idx].rect;
        return captureRegion(rc, idx, "d", settings, timestamp, frameNumber, includeFrame);
    }

    bool captureAll(const AppSettings& settings, std::time_t timestamp, int frameNumber, bool includeFrame) {
        bool allSuccess = true;
        for (const auto& d : displays) {
            std::cout << "Capturing display " << d.index << " (" << d.name << ")..." << std::endl;
            std::string label = std::to_string(d.index + 1);
            if (!captureRegion(d.rect, d.index, label, settings, timestamp, frameNumber, includeFrame)) {
                allSuccess = false;
            }
        }
        return allSuccess;
    }

    bool captureSubset(const std::vector<int>& indices, const AppSettings& settings, std::time_t timestamp, int frameNumber, bool includeFrame) {
        bool allSuccess = true;
        for (int idx : indices) {
            if (idx < 0 || idx >= static_cast<int>(displays.size())) {
                std::cerr << "Invalid display index: " << (idx + 1) << std::endl;
                allSuccess = false;
                continue;
            }
            const auto& d = displays[idx];
            std::cout << "Capturing display " << (idx + 1) << " (" << d.name << ")..." << std::endl;
            std::string label = std::to_string(idx + 1);
            if (!captureRegion(d.rect, idx, label, settings, timestamp, frameNumber, includeFrame)) {
                allSuccess = false;
            }
        }
        return allSuccess;
    }

private:
    bool captureRegion(const RECT& rect, int displayIndex, const std::string& displayLabel,
                       const AppSettings& settings, std::time_t timestamp, int frameNumber, bool includeFrame) {
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) {
            std::cerr << "Invalid dimensions for display " << displayIndex << std::endl;
            return false;
        }

        HDC screenDC = GetDC(nullptr);
        if (!screenDC) {
            std::cerr << "Failed to get screen DC." << std::endl;
            return false;
        }

        HDC memDC = CreateCompatibleDC(screenDC);
        if (!memDC) {
            std::cerr << "Failed to create memory DC." << std::endl;
            ReleaseDC(nullptr, screenDC);
            return false;
        }

        HBITMAP bmp = CreateCompatibleBitmap(screenDC, width, height);
        if (!bmp) {
            std::cerr << "Failed to create bitmap." << std::endl;
            DeleteDC(memDC);
            ReleaseDC(nullptr, screenDC);
            return false;
        }

        HGDIOBJ oldBmp = SelectObject(memDC, bmp);
        BOOL bltOk = BitBlt(memDC, 0, 0, width, height, screenDC, rect.left, rect.top, SRCCOPY);
        SelectObject(memDC, oldBmp);

        if (!bltOk) {
            std::cerr << "BitBlt failed for display " << displayIndex << std::endl;
            DeleteObject(bmp);
            DeleteDC(memDC);
            ReleaseDC(nullptr, screenDC);
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;

        int rowStride = ((width * 3 + 3) & ~3);
        std::vector<uint8_t> buffer(rowStride * height);

        if (!GetDIBits(memDC, bmp, 0, static_cast<UINT>(height), buffer.data(), &bmi, DIB_RGB_COLORS)) {
            std::cerr << "GetDIBits failed for display " << displayIndex << std::endl;
            DeleteObject(bmp);
            DeleteDC(memDC);
            ReleaseDC(nullptr, screenDC);
            return false;
        }

        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);

        std::wstring outputDir = utf8ToWide(settings.outputDir);
        if (settings.openAfterCapture && !settings.saveBeforeOpen) {
            // Keep the file after launch: Photos may load it asynchronously, and editors can save back to it.
            outputDir = (fs::temp_directory_path() / L"TCapture").wstring();
            fs::create_directories(fs::path(outputDir));
        }
        std::wstring filename = generateFilename(displayLabel, outputDir, settings.format, timestamp, frameNumber, includeFrame);
        if (filename.empty()) {
            std::cerr << "Failed to build output path." << std::endl;
            return false;
        }
        if (settings.openAfterCapture) filename = fs::absolute(fs::path(filename)).wstring();
        std::wstring savedFilename;
        int effectiveCompression = (toLower(settings.format) == "png") ? settings.pngCompression : settings.jpgQuality;
        if (!saveImageWic(filename, width, height, buffer, rowStride, settings.format, effectiveCompression, savedFilename)) {
            std::cerr << "Failed to save file: " << wideToUtf8(filename) << std::endl;
            return false;
        }

        if (settings.openAfterCapture) openedImages.push_back(savedFilename);
        std::error_code ec;
        auto fileSize = fs::file_size(fs::path(savedFilename), ec);
        double kb = ec ? 0.0 : static_cast<double>(fileSize) / 1024.0;
        std::cout << "Saved: " << wideToUtf8(savedFilename);
        if (!ec) {
            std::cout << " (" << std::fixed << std::setprecision(1) << kb << " KB)";
        }
        std::cout << std::endl;
        return true;
    }
};

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  -l, --list              List displays\n"
              << "  -d, --display IDS       Capture displays (1-based). Examples:\n"
              << "                            0 or all => all displays (default)\n"
              << "                            1        => only display 1\n"
              << "                            1,3      => displays 1 and 3\n"
              << "                            active_display => active display (foreground window's monitor)\n"
              << "                            active_window  => active window\n"
              << "  -a, --all               Capture all displays (default)\n"
              << "  -o, --output DIR        Output directory (default/TCapture.ini)\n"
              << "  -f, --format fmt        png or jpg (default/TCapture.ini)\n"
              << "  -q, --quality N         PNG: compression 0-9, JPG: quality 1-100\n"
              << "      --fps N             Burst FPS (0=off, 1-60). 0 behaves as single capture.\n"
              << "      --duration N        Burst duration in seconds (used when --fps>0)\n"
              << "  -p, --profile NAME      Load settings from [NAME] section in TCapture.ini\n"
              << "  -h, --help, help, /?    Show this help\n\n"
              << "Examples:\n"
              << "  " << program << " --list\n"
              << "  " << program << " -a -o C:\\\\tmp\n"
              << "  " << program << " -d 1,3 -o C:\\\\captures -f jpg -q 90\n"
              << "  " << program << " --fps 10 --duration 3\n"
              << "\n"
              << "TCapture.ini keys (optional):\n"
              << "  output_dir=.\n"
              << "  format=png\n"
              << "  compression=6   (png: 0-9, jpg: 1-100)\n"
              << "  burst_fps=0     (0=off, 1-60)\n"
              << "  burst_seconds=1 (used when burst_fps>0)\n"
              << "  displays=all    (all,0,* or 1,3,...)\n"
              << "  hotkey_capture=Ctrl+Alt+S (used by TCapture.exe --agent)\n"
              << "  [profile_name]  (multiple sections allowed; use --profile to choose)\n"
              << std::endl;
}

int RunCaptureMain(int argc, char* argv[]) {
    auto enableDpiAware = []() {
        // Best effort DPI awareness for per-monitor scaling.
        using SetDpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            auto setCtx = reinterpret_cast<SetDpiContextFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            if (setCtx) {
                if (setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
                    return;
                }
            }
        }
        // Fallback for older systems.
        HMODULE shcore = LoadLibraryW(L"Shcore.dll");
        if (shcore) {
            using SetDpiAwarenessFn = HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS);
            auto setAwareness = reinterpret_cast<SetDpiAwarenessFn>(
                GetProcAddress(shcore, "SetProcessDpiAwareness"));
            if (setAwareness) {
                setAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
            }
            FreeLibrary(shcore);
        } else {
            SetProcessDPIAware();
        }
    };

    struct ComGuard {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~ComGuard() { if (SUCCEEDED(result)) CoUninitialize(); }
    } comGuard;

    try {
        enableDpiAware();

        bool listMode = false;
        bool captureAllDisplays = true;
        std::string profileName = "default";
        bool cliDisplaySet = false;
        std::vector<int> selectedDisplays;
        bool cliActiveDisplay = false;
        bool cliActiveWindow = false;
        bool outputOverride = false;
        bool formatOverride = false;
        bool compressionOverride = false;
        bool burstFpsOverride = false;
        bool burstSecondsOverride = false;
        std::string outputValue;
        std::string formatValue;
        int compressionValue = 0;
        int burstFpsValue = 0;
        int burstSecondsValue = 0;

        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];

            if (arg == "--capture") {
                // Explicit capture mode marker for unified executable.
                continue;
            } else if (arg == "-l" || arg == "--list") {
                listMode = true;
            } else if (arg == "-a" || arg == "--all") {
                captureAllDisplays = true;
                cliDisplaySet = true;
                selectedDisplays.clear();
            } else if (arg == "-d" || arg == "--display") {
                if (i + 1 < argc) {
                    std::string listArg = argv[++i];
                    bool parsedAll = false;
                    bool activeDisplay = false;
                    bool activeWindow = false;
                    selectedDisplays = parseDisplayList(listArg, parsedAll, activeDisplay, activeWindow);
                    captureAllDisplays = parsedAll || (!activeDisplay && !activeWindow && selectedDisplays.empty());
                    cliActiveDisplay = activeDisplay;
                    cliActiveWindow = activeWindow;
                    cliDisplaySet = true;
                } else {
                    std::cerr << "Option -d requires an argument." << std::endl;
                    return 1;
                }
            } else if (arg == "-o" || arg == "--output") {
                if (i + 1 < argc) {
                    outputOverride = true;
                    outputValue = argv[++i];
                } else {
                    std::cerr << "Option -o requires an argument." << std::endl;
                    return 1;
                }
            } else if (arg == "-f" || arg == "--format") {
                if (i + 1 < argc) {
                    formatOverride = true;
                    formatValue = argv[++i];
                } else {
                    std::cerr << "Option -f requires an argument." << std::endl;
                    return 1;
                }
            } else if (arg == "-q" || arg == "--quality") {
                if (i + 1 < argc) {
                    try {
                        compressionValue = std::stoi(argv[++i]);
                        compressionOverride = true;
                    } catch (...) {
                        std::cerr << "Invalid value for --quality." << std::endl;
                        return 1;
                    }
                } else {
                    std::cerr << "Option -q requires an argument." << std::endl;
                    return 1;
                }
            } else if (arg == "--fps") {
                if (i + 1 < argc) {
                    try {
                        burstFpsValue = std::stoi(argv[++i]);
                        burstFpsOverride = true;
                    } catch (...) {
                        std::cerr << "Invalid value for --fps." << std::endl;
                        return 1;
                    }
                } else {
                    std::cerr << "Option --fps requires an argument." << std::endl;
                    return 1;
                }
            } else if (arg == "--duration" || arg == "--seconds") {
                if (i + 1 < argc) {
                    try {
                        burstSecondsValue = std::stoi(argv[++i]);
                        burstSecondsOverride = true;
                    } catch (...) {
                        std::cerr << "Invalid value for --duration/--seconds." << std::endl;
                        return 1;
                    }
                } else {
                    std::cerr << "Option --duration requires an argument." << std::endl;
                    return 1;
                }
            } else if (arg == "-p" || arg == "--profile") {
                if (i + 1 < argc) {
                    profileName = argv[++i];
                } else {
                    std::cerr << "Option -p requires an argument." << std::endl;
                    return 1;
                }
            } else if (arg == "-h" || arg == "--help" || arg == "help" || arg == "/?") {
                printUsage(argv[0]);
                return 0;
            } else {
                std::cerr << "Unknown option: " << arg << std::endl;
                printUsage(argv[0]);
                return 1;
            }
        }

        AppSettings settings;
        // Prefer TCapture.ini alongside the executable; fall back to CWD.
        fs::path exeDir = getExecutableDir();
        fs::path exeIni = exeDir / "TCapture.ini";
        bool loaded = loadSettings(settings, exeIni, profileName);
        if (!loaded) {
            loaded = loadSettings(settings, "TCapture.ini", profileName);
        }
        normalizeSettings(settings);

        if (outputOverride) settings.outputDir = outputValue;
        if (formatOverride) settings.format = formatValue;
        if (compressionOverride) {
            settings.compression = compressionValue;
            settings.pngCompression = compressionValue;
            settings.jpgQuality = compressionValue;
        }
        if (burstFpsOverride) settings.burstFps = burstFpsValue;
        if (burstSecondsOverride) settings.burstSeconds = burstSecondsValue;
        normalizeSettings(settings);

        // Apply display selection preference with overrides.
        if (cliDisplaySet) {
            if (cliActiveWindow) {
                settings.captureActiveWindow = true;
                settings.captureActiveDisplay = false;
                captureAllDisplays = false;
                selectedDisplays.clear();
            } else if (cliActiveDisplay) {
                settings.captureActiveDisplay = true;
                settings.captureActiveWindow = false;
                captureAllDisplays = false;
                selectedDisplays.clear();
            } else if (!selectedDisplays.empty()) {
                settings.captureActiveWindow = false;
                settings.captureActiveDisplay = false;
                captureAllDisplays = false;
            } else {
                captureAllDisplays = true;
            }
        } else {
            // If TCapture.ini specified displays and CLI did not override, apply them.
            if (settings.captureActiveWindow) {
                captureAllDisplays = false;
                selectedDisplays.clear();
            } else if (settings.captureActiveDisplay) {
                captureAllDisplays = false;
                selectedDisplays.clear();
            } else if (!settings.displayIndices.empty()) {
                selectedDisplays = settings.displayIndices;
                captureAllDisplays = false;
            }
        }

        if ((!settings.openAfterCapture || settings.saveBeforeOpen) && !settings.outputDir.empty() && settings.outputDir != ".") {
            fs::path outPath(utf8ToWide(settings.outputDir));
            if (!fs::exists(outPath)) {
                std::cout << "Creating output directory: " << wideToUtf8(outPath.wstring()) << std::endl;
                fs::create_directories(outPath);
            }
        }

        ScreenshotTool tool;

        if (!tool.detectDisplays()) {
            std::cerr << "No displays detected." << std::endl;
            return 1;
        }

        if (listMode) {
            tool.listDisplays();
            return 0;
        }

        auto captureOnce = [&](std::time_t timestamp, int frameNumber, bool includeFrame) -> bool {
            if (settings.captureActiveWindow) {
                return tool.captureActiveWindow(settings, timestamp, frameNumber, includeFrame);
            }
            if (settings.captureActiveDisplay) {
                return tool.captureActiveDisplay(settings, timestamp, frameNumber, includeFrame);
            }
            if (captureAllDisplays) {
                return tool.captureAll(settings, timestamp, frameNumber, includeFrame);
            }
            return tool.captureSubset(selectedDisplays, settings, timestamp, frameNumber, includeFrame);
        };

        bool success = false;
        if (settings.burstFps <= 0) {
            std::time_t nowSec = std::time(nullptr);
            success = captureOnce(nowSec, 1, false);
        } else {
            int fps = settings.burstFps;
            auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / static_cast<double>(fps)));
            if (interval.count() <= 0) {
                interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / 60.0));
            }
            auto start = std::chrono::steady_clock::now();
            auto deadline = start + std::chrono::seconds(settings.burstSeconds);
            auto nextFrame = start;
            std::time_t currentSecond = std::time(nullptr);
            int frameInSecond = 0;
            success = true;
            std::cout << "Burst mode: " << fps << " fps for " << settings.burstSeconds << " second(s)." << std::endl;
            while (true) {
                auto now = std::chrono::steady_clock::now();
                if (now > deadline) break;
                std::time_t nowSec = std::time(nullptr);
                if (nowSec != currentSecond) {
                    currentSecond = nowSec;
                    frameInSecond = 0;
                }
                ++frameInSecond;
                if (!captureOnce(currentSecond, frameInSecond, true)) success = false;
                nextFrame += interval;
                now = std::chrono::steady_clock::now();
                if (nextFrame > deadline) break;
                if (nextFrame > now) {
                    std::this_thread::sleep_until(nextFrame);
                }
            }
        }

        // Complete all captures before opening apps so they cannot appear in later monitor images.
        if (settings.openAfterCapture && !tool.openImages(settings)) success = false;
        return success ? 0 : 1;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

#ifdef MSC_CAPTURE_STANDALONE
int main(int argc, char* argv[]) {
    return RunCaptureMain(argc, argv);
}

#ifdef _WIN32
// Windows GUI entry point to avoid spawning a console window.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return RunCaptureMain(__argc, __argv);
}
#endif
#endif
