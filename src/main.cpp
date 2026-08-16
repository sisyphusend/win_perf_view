// PerfOverlay - a tiny always-on-top HUD showing CPU / RAM / GPU usage.
// Pure Win32 + GDI + PDH. No .NET, no third-party libs, no DLL injection.
//
// Drag   : left-click and drag anywhere on the HUD to move it.
// Menu   : right-click the HUD or the tray icon -> Hide/Show, layout,
//          background, lock position, start with Windows, Settings, Exit.
//          Left-click the tray icon to toggle Hide/Show instantly.
// Lock   : makes the HUD click-through (mouse passes to the window below).
//          While locked, only the tray icon's menu can unlock/control it.
// Settings window (menu -> Settings...) also exposes: per-row history
// sparklines (vertical layout only), global hotkeys (Ctrl+Alt+O
// Show/Hide, Ctrl+Alt+L Lock/Unlock), and auto-hiding whenever a
// fullscreen app/game has focus.
// Every toggle and the window position are saved to
// HKCU\Software\PerfOverlay and restored on the next launch.

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cwchar>
#include <algorithm>
#include "resource.h"

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace {

constexpr UINT_PTR TIMER_ID = 1;
constexpr UINT TIMER_INTERVAL_MS = 1000;
constexpr UINT ID_MENU_TOGGLE = 1000;
constexpr UINT ID_MENU_EXIT = 1001;
constexpr UINT ID_MENU_LAYOUT = 1002;
constexpr UINT ID_MENU_BG = 1003;
constexpr UINT ID_MENU_LOCK = 1004;
constexpr UINT ID_MENU_AUTOSTART = 1005;
constexpr UINT ID_MENU_SETTINGS = 1006;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT TRAY_ICON_ID = 1;

constexpr int HOTKEY_ID_TOGGLE = 1; // Ctrl+Alt+O: Show/Hide
constexpr int HOTKEY_ID_LOCK = 2;   // Ctrl+Alt+L: Lock/Unlock

bool g_hudVisible = true;
bool g_horizontalMode = false; // false = stacked rows (default), true = one line
bool g_showBg = false;         // false = fully transparent (default), true = solid card
bool g_locked = false;         // true = click-through, mouse passes to the window below
bool g_showSparkline = true;   // vertical mode only: small history graph per row
bool g_hotkeyToggleEnabled = false;
bool g_hotkeyLockEnabled = false;
bool g_autoHideFullscreen = false; // hide the HUD while a game/app is exclusive-fullscreen
bool g_autoHiddenByFullscreen = false; // true while we (not the user) are hiding it

HWND g_mainHwnd = nullptr;
HWND g_settingsHwnd = nullptr;
HINSTANCE g_hInstance = nullptr;

// Layout constants. Vertical-mode rows are label | progress bar | value,
// columns aligned across rows; horizontal mode stays a compact single line.
constexpr int PAD = 14;
constexpr int ROW_H = 40;              // vertical mode: row-to-row spacing
constexpr int ROW_CONTENT_H = 18;      // vertical mode: text/bar vertical-centering reference
constexpr int INLINE_ROW_H = 18;       // horizontal mode: single line content height
constexpr int ROW_GAP = 10;            // horizontal mode: gap between label and value
constexpr int COL_GAP = 14;            // vertical mode: gap either side of the bar
constexpr int ITEM_GAP = 14;           // horizontal mode: padding either side of a divider
constexpr int DIVIDER_W = 1;
constexpr int WND_MIN_W = 170;
constexpr int WND_MAX_W_VERTICAL = 460;
constexpr int WND_MAX_W_HORIZONTAL = 1000; // wide enough that real GPU names/multi-GPU never get clipped
constexpr int SPARK_W = 36;            // vertical mode, when sparklines replace the bar
constexpr int SPARK_H = 12;
constexpr size_t HISTORY_LEN = 40;     // ~40s of samples at 1 tick/s
constexpr int BAR_TRACK_H = 4;         // vertical mode: progress bar thickness
constexpr int BAR_HANDLE_D = 9;        // vertical mode: dot at the fill's leading edge
constexpr int PREFERRED_BAR_W = 100;   // used only to pick the window's natural width
constexpr int MIN_BAR_W = 40;
constexpr int CORNER_RADIUS = 14;      // solid-background mode only

NOTIFYICONDATAW g_trayIcon{};
bool g_trayIconAdded = false;

PDH_HQUERY g_cpuQuery = nullptr;
PDH_HCOUNTER g_cpuCounter = nullptr;

// One PDH "GPU Engine" counter instance, tagged with which physical adapter
// (by LUID, parsed out of the instance path) and engine type it belongs to.
struct GpuCounterEntry {
    PDH_HCOUNTER counter;
    std::wstring engType;
    UINT64 luid;
};
PDH_HQUERY g_gpuQuery = nullptr;
std::unordered_map<std::wstring, GpuCounterEntry> g_gpuCounters;
int g_tickCount = 0;

// One row shown in the HUD per physical GPU (name comes from DXGI).
struct GpuStat {
    UINT64 luid;
    std::wstring name;
    double percent;
    std::vector<float> history;
};
std::vector<GpuStat> g_gpuStats;
bool g_gpuSingleFallback = false; // true if DXGI enumeration found no adapters

double g_cpuPercent = 0.0;
double g_ramPercent = 0.0;
double g_ramUsedGB = 0.0;
double g_ramTotalGB = 0.0;
std::vector<float> g_cpuHistory;
std::vector<float> g_ramHistory;

// Fixed-length rolling window: drop the oldest sample once full. HISTORY_LEN
// is small (40) so the erase-front is a trivial memmove, not worth a ring
// buffer.
void PushHistory(std::vector<float>& hist, double value) {
    hist.push_back(static_cast<float>(value));
    if (hist.size() > HISTORY_LEN) hist.erase(hist.begin());
}

HFONT g_fontLabel = nullptr;
HFONT g_fontValue = nullptr;

// --- metric collection -----------------------------------------------

void InitCpuCounter() {
    if (PdhOpenQueryW(nullptr, 0, &g_cpuQuery) != ERROR_SUCCESS) return;
    // "% Processor Time" is raw busy time and can read far lower than Task
    // Manager whenever the CPU is running below its nominal clock (power
    // saving) or boosting above it. Task Manager itself shows "% Processor
    // Utility", which folds in the current performance state - use the same
    // counter so the HUD's number matches what Task Manager shows.
    PdhAddEnglishCounterW(g_cpuQuery, L"\\Processor Information(_Total)\\% Processor Utility", 0, &g_cpuCounter);
    PdhCollectQueryData(g_cpuQuery);
}

void UpdateCpu() {
    if (!g_cpuQuery) return;
    if (PdhCollectQueryData(g_cpuQuery) != ERROR_SUCCESS) return;
    PDH_FMT_COUNTERVALUE value;
    if (PdhGetFormattedCounterValue(g_cpuCounter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS) {
        if (value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA) {
            g_cpuPercent = std::max(0.0, std::min(100.0, value.doubleValue));
        }
    }
}

void UpdateRam() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        g_ramPercent = status.dwMemoryLoad;
        g_ramTotalGB = static_cast<double>(status.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
        g_ramUsedGB = g_ramTotalGB - static_cast<double>(status.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    }
}

std::wstring ExtractEngType(const std::wstring& path) {
    size_t pos = path.find(L"engtype_");
    if (pos == std::wstring::npos) return L"unknown";
    pos += 8;
    size_t end = path.find(L')', pos);
    if (end == std::wstring::npos) end = path.size();
    return path.substr(pos, end - pos);
}

// GPU Engine instance names look like:
//   pid_1234_luid_0x00000000_0x0000D3E6_phys_0_eng_0_engtype_3D
// Pull the LUID back out so each engine instance can be attributed to the
// physical adapter DXGI told us about.
bool ParseLuidFromPath(const std::wstring& path, UINT64& outLuid) {
    size_t pos = path.find(L"luid_0x");
    if (pos == std::wstring::npos) return false;
    pos += 7;
    size_t sep = path.find(L'_', pos);
    if (sep == std::wstring::npos) return false;
    std::wstring highStr = path.substr(pos, sep - pos);

    size_t lowStart = sep + 1;
    if (path.compare(lowStart, 2, L"0x") != 0) return false;
    lowStart += 2;
    size_t lowEnd = path.find(L'_', lowStart);
    if (lowEnd == std::wstring::npos) return false;
    std::wstring lowStr = path.substr(lowStart, lowEnd - lowStart);

    DWORD high = wcstoul(highStr.c_str(), nullptr, 16);
    DWORD low = wcstoul(lowStr.c_str(), nullptr, 16);
    outLuid = (static_cast<UINT64>(high) << 32) | low;
    return true;
}

UINT64 PackLuid(LONG highPart, DWORD lowPart) {
    return (static_cast<UINT64>(static_cast<DWORD>(highPart)) << 32) | lowPart;
}

// Strips vendor/legal noise from a DXGI adapter description while keeping
// the actual model name intact (no fixed-length truncation - the HUD sizes
// itself to whatever this returns instead of cutting names off).
std::wstring ShortenGpuName(std::wstring name) {
    auto strip = [&](const wchar_t* token) {
        size_t pos;
        size_t len = wcslen(token);
        while ((pos = name.find(token)) != std::wstring::npos) name.erase(pos, len);
    };
    strip(L"(R)");
    strip(L"(TM)");
    strip(L"Corporation ");
    strip(L"NVIDIA ");
    strip(L"AMD ");
    strip(L"Intel ");

    auto stripSuffix = [&](const wchar_t* suffix) {
        size_t len = wcslen(suffix);
        if (name.size() >= len && name.compare(name.size() - len, len, suffix) == 0) {
            name.erase(name.size() - len, len);
        }
    };
    for (int pass = 0; pass < 2; ++pass) {
        stripSuffix(L" Laptop GPU");
        stripSuffix(L" Graphics Card");
        stripSuffix(L" Graphics Family");
        stripSuffix(L" Graphics");
        stripSuffix(L" Family");
        while (!name.empty() && name.back() == L' ') name.pop_back();
    }

    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    size_t p;
    while ((p = name.find(L"  ")) != std::wstring::npos) name.erase(p, 1);

    return name.empty() ? L"GPU" : name;
}

// Enumerate physical (non-software) display adapters via DXGI so the HUD can
// show each GPU's real name. One-time startup cost only.
std::vector<GpuStat> EnumerateGpuAdapters() {
    std::vector<GpuStat> result;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return result;
    }

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            GpuStat stat;
            stat.luid = PackLuid(desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
            stat.name = ShortenGpuName(desc.Description);
            stat.percent = 0.0;
            result.push_back(stat);
        }
        adapter->Release();
        adapter = nullptr;
    }

    factory->Release();
    return result;
}

// GPU usage isn't exposed as a single counter: Windows publishes one
// "engine" instance per (process, adapter, engine-type) pair that appears
// and disappears as work happens. Re-expanding the wildcard and rebuilding
// the whole query every tick is expensive (~8% CPU measured), so instead we
// keep one persistent query and only rescan for new/gone instances every
// few seconds; every tick just collects the counters we already have.
void InitGpuQuery() {
    PdhOpenQueryW(nullptr, 0, &g_gpuQuery);
}

void RefreshGpuInstances() {
    if (!g_gpuQuery) return;
    static const wchar_t* kWildcardPath = L"\\GPU Engine(*)\\Utilization Percentage";

    DWORD pathListLen = 0;
    PDH_STATUS status = PdhExpandWildCardPathW(nullptr, kWildcardPath, nullptr, &pathListLen, 0);
    if (status != PDH_MORE_DATA || pathListLen == 0) return;

    std::vector<wchar_t> pathList(pathListLen);
    status = PdhExpandWildCardPathW(nullptr, kWildcardPath, pathList.data(), &pathListLen, 0);
    if (status != ERROR_SUCCESS) return;

    std::unordered_map<std::wstring, bool> seen;
    for (const wchar_t* p = pathList.data(); *p != L'\0'; p += wcslen(p) + 1) {
        std::wstring path(p);
        seen[path] = true;
        if (g_gpuCounters.find(path) == g_gpuCounters.end()) {
            PDH_HCOUNTER counter = nullptr;
            if (PdhAddCounterW(g_gpuQuery, path.c_str(), 0, &counter) == ERROR_SUCCESS) {
                GpuCounterEntry entry;
                entry.counter = counter;
                entry.engType = ExtractEngType(path);
                UINT64 luid = 0;
                ParseLuidFromPath(path, luid);
                entry.luid = luid;
                g_gpuCounters[path] = entry;
            }
        }
    }

    for (auto it = g_gpuCounters.begin(); it != g_gpuCounters.end();) {
        if (seen.find(it->first) == seen.end()) {
            PdhRemoveCounter(it->second.counter);
            it = g_gpuCounters.erase(it);
        } else {
            ++it;
        }
    }
}

// Per adapter: takes the max utilization per engine-type (matches how Task
// Manager rolls these up), then the max across engine-types as that GPU's
// overall load. If DXGI found no adapters, falls back to one aggregate row
// across every engine instance regardless of LUID.
void UpdateGpu() {
    if (!g_gpuQuery || g_gpuCounters.empty()) {
        for (auto& stat : g_gpuStats) stat.percent = 0.0;
        return;
    }
    if (PdhCollectQueryData(g_gpuQuery) != ERROR_SUCCESS) return;

    if (g_gpuSingleFallback) {
        std::unordered_map<std::wstring, double> maxByEngType;
        for (auto& [path, entry] : g_gpuCounters) {
            PDH_FMT_COUNTERVALUE value;
            if (PdhGetFormattedCounterValue(entry.counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS) continue;
            if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            double v = std::max(0.0, value.doubleValue);
            auto it = maxByEngType.find(entry.engType);
            if (it == maxByEngType.end() || v > it->second) maxByEngType[entry.engType] = v;
        }
        double overall = 0.0;
        for (auto& [engType, v] : maxByEngType) overall = std::max(overall, v);
        if (!g_gpuStats.empty()) g_gpuStats[0].percent = std::min(100.0, overall);
        return;
    }

    std::unordered_map<UINT64, std::unordered_map<std::wstring, double>> perGpu;
    for (auto& [path, entry] : g_gpuCounters) {
        PDH_FMT_COUNTERVALUE value;
        if (PdhGetFormattedCounterValue(entry.counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS) continue;
        if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        double v = std::max(0.0, value.doubleValue);
        auto& m = perGpu[entry.luid];
        auto it = m.find(entry.engType);
        if (it == m.end() || v > it->second) m[entry.engType] = v;
    }

    for (auto& stat : g_gpuStats) {
        double overall = 0.0;
        auto it = perGpu.find(stat.luid);
        if (it != perGpu.end()) {
            for (auto& [engType, v] : it->second) overall = std::max(overall, v);
        }
        stat.percent = std::min(100.0, overall);
    }
}

// --- rendering ----------------------------------------------------------

// By default the window has no background box: kColorKey is painted behind
// everything, then declared as the layered window's transparent color, so
// only the text/dividers are visible - floating directly over the desktop.
// It must never match a color we actually draw with. g_showBg swaps that
// fill for an opaque card instead (see OnPaint).
constexpr COLORREF kColorKey = RGB(1, 1, 2);
constexpr COLORREF kColorBg = RGB(14, 14, 17);
constexpr COLORREF kColorDivider = RGB(110, 110, 122);
constexpr COLORREF kColorLabelText = RGB(220, 220, 227);
constexpr COLORREF kColorAccent = RGB(46, 214, 224); // single accent for every bar/value, not severity-colored
constexpr COLORREF kColorTrack = RGB(46, 46, 53);    // progress bar's unfilled track

int TextWidth(HDC hdc, HFONT font, const std::wstring& text) {
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
    SIZE sz{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(hdc, old);
    return sz.cx;
}

// "GPU 0 · GeForce RTX 3050" - numbered so multiple GPUs stay distinguishable
// even if two happen to share a name, with the full (never-truncated) model
// name after the separator.
std::wstring GpuRowLabel(size_t index, const std::wstring& name) {
    wchar_t buf[80];
    swprintf_s(buf, L"GPU %d · %ls", static_cast<int>(index), name.c_str());
    return buf;
}

void ComputeColumnWidths(HDC dc, int& labelColW, int& valueColW) {
    wchar_t ramLabel[48];
    swprintf_s(ramLabel, L"RAM %.1f/%.1fGB", g_ramUsedGB, g_ramTotalGB);
    labelColW = std::max(TextWidth(dc, g_fontLabel, L"CPU"), TextWidth(dc, g_fontLabel, ramLabel));
    for (size_t i = 0; i < g_gpuStats.size(); ++i) {
        labelColW = std::max(labelColW, TextWidth(dc, g_fontLabel, GpuRowLabel(i, g_gpuStats[i].name)));
    }
    valueColW = TextWidth(dc, g_fontValue, L"100%");
}

// Small fixed-width line plot of the last HISTORY_LEN samples (0-100 scaled
// to the given height). x-axis is scaled against HISTORY_LEN, not the
// buffer's current size, so the trace starts at the left edge and fills
// rightward as samples accumulate rather than stretching to fit.
void DrawSparkline(HDC hdc, int x, int y, int w, int h, const std::vector<float>& history, COLORREF color) {
    if (history.size() < 2) return;
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));

    size_t n = history.size();
    for (size_t i = 0; i < n; ++i) {
        int px = x + static_cast<int>(static_cast<double>(i) / (HISTORY_LEN - 1) * w);
        double v = std::max(0.0, std::min(100.0, static_cast<double>(history[i])));
        int py = y + h - static_cast<int>(v / 100.0 * h);
        if (i == 0) MoveToEx(hdc, px, py, nullptr);
        else LineTo(hdc, px, py);
    }

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

// Rounded-capsule progress bar with a dot handle at the fill's leading edge
// (like a slider thumb). yCenter is the bar's vertical midline.
void DrawBar(HDC hdc, int x, int yCenter, int w, double percent) {
    percent = std::max(0.0, std::min(100.0, percent));
    int trackTop = yCenter - BAR_TRACK_H / 2;

    HBRUSH trackBrush = CreateSolidBrush(kColorTrack);
    HPEN trackPen = CreatePen(PS_SOLID, 1, kColorTrack);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, trackBrush));
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, trackPen));
    RoundRect(hdc, x, trackTop, x + w, trackTop + BAR_TRACK_H, BAR_TRACK_H, BAR_TRACK_H);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(trackBrush);
    DeleteObject(trackPen);

    int fillW = static_cast<int>(w * (percent / 100.0));
    if (fillW > BAR_TRACK_H) {
        HBRUSH fillBrush = CreateSolidBrush(kColorAccent);
        HPEN fillPen = CreatePen(PS_SOLID, 1, kColorAccent);
        HBRUSH oldFillBrush = static_cast<HBRUSH>(SelectObject(hdc, fillBrush));
        HPEN oldFillPen = static_cast<HPEN>(SelectObject(hdc, fillPen));
        RoundRect(hdc, x, trackTop, x + fillW, trackTop + BAR_TRACK_H, BAR_TRACK_H, BAR_TRACK_H);
        SelectObject(hdc, oldFillBrush);
        SelectObject(hdc, oldFillPen);
        DeleteObject(fillBrush);
        DeleteObject(fillPen);
    }

    int handleX = x + fillW;
    HBRUSH dotBrush = CreateSolidBrush(kColorAccent);
    HPEN dotPen = CreatePen(PS_SOLID, 1, kColorAccent);
    HBRUSH oldDotBrush = static_cast<HBRUSH>(SelectObject(hdc, dotBrush));
    HPEN oldDotPen = static_cast<HPEN>(SelectObject(hdc, dotPen));
    Ellipse(hdc, handleX - BAR_HANDLE_D / 2, yCenter - BAR_HANDLE_D / 2, handleX + BAR_HANDLE_D / 2,
            yCenter + BAR_HANDLE_D / 2);
    SelectObject(hdc, oldDotBrush);
    SelectObject(hdc, oldDotPen);
    DeleteObject(dotBrush);
    DeleteObject(dotPen);
}

// Draws one row: label | progress bar (or sparkline, if enabled - the two
// are mutually exclusive to avoid clutter) | value, three columns aligned
// across every row via the labelColW/valueColW/barW the caller computed.
void DrawRow(HDC hdc, int x, int y, int labelColW, int barW, int valueColW, const wchar_t* label, double percent,
             const std::vector<float>* history) {
    percent = std::max(0.0, std::min(100.0, percent));

    SelectObject(hdc, g_fontLabel);
    SetTextColor(hdc, kColorLabelText);
    SIZE labelSz{};
    GetTextExtentPoint32W(hdc, label, static_cast<int>(wcslen(label)), &labelSz);
    TextOutW(hdc, x, y + (ROW_CONTENT_H - labelSz.cy) / 2, label, static_cast<int>(wcslen(label)));

    int barX = x + labelColW + COL_GAP;
    if (g_showSparkline && history) {
        DrawSparkline(hdc, barX, y + (ROW_CONTENT_H - SPARK_H) / 2, barW, SPARK_H, *history, kColorAccent);
    } else {
        DrawBar(hdc, barX, y + ROW_CONTENT_H / 2, barW, percent);
    }

    wchar_t valBuf[16];
    swprintf_s(valBuf, L"%.0f%%", percent);
    SelectObject(hdc, g_fontValue);
    SIZE valSz{};
    GetTextExtentPoint32W(hdc, valBuf, static_cast<int>(wcslen(valBuf)), &valSz);
    int valueX = barX + barW + COL_GAP + (valueColW - valSz.cx);
    SetTextColor(hdc, kColorAccent);
    TextOutW(hdc, valueX, y + (ROW_CONTENT_H - valSz.cy) / 2, valBuf, static_cast<int>(wcslen(valBuf)));
}

// Horizontal-mode variant: "label value%" packed tightly left-to-right (no
// right-alignment against a row width). The value is right-aligned within a
// slot sized to "100%" - the widest possible reading - and always returns
// that slot's full width as consumed, regardless of the actual digit count.
// Otherwise "9%" vs "34%" vs "100%" are different pixel widths, and every
// divider/item after this one would shift left-right as the number changes.
int DrawMetricInline(HDC hdc, int x, int y, const wchar_t* label, double percent) {
    percent = std::max(0.0, std::min(100.0, percent));

    SelectObject(hdc, g_fontLabel);
    SIZE labelSz{};
    GetTextExtentPoint32W(hdc, label, static_cast<int>(wcslen(label)), &labelSz);
    SetTextColor(hdc, kColorLabelText);
    TextOutW(hdc, x, y, label, static_cast<int>(wcslen(label)));

    SelectObject(hdc, g_fontValue);
    SIZE slotSz{};
    GetTextExtentPoint32W(hdc, L"100%", 4, &slotSz);
    int valueSlotX = x + labelSz.cx + ROW_GAP / 2;

    wchar_t valBuf[16];
    swprintf_s(valBuf, L"%.0f%%", percent);
    SIZE valSz{};
    GetTextExtentPoint32W(hdc, valBuf, static_cast<int>(wcslen(valBuf)), &valSz);
    SetTextColor(hdc, kColorAccent);
    TextOutW(hdc, valueSlotX + (slotSz.cx - valSz.cx), y - 3, valBuf, static_cast<int>(wcslen(valBuf)));

    return (valueSlotX + slotSz.cx) - x;
}

// Thin vertical divider between horizontal-mode blocks, matching the
// "FPS N/A | GPU 0% | CPU 3%" reference style.
int DrawDivider(HDC hdc, int x, int contentH) {
    int dx = x + ITEM_GAP;
    HPEN pen = CreatePen(PS_SOLID, DIVIDER_W, kColorDivider);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, dx, PAD - 1, nullptr);
    LineTo(hdc, dx, PAD + contentH + 1);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    return dx + ITEM_GAP;
}

void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right;
    int height = rc.bottom;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

    if (g_showBg) {
        // Opaque solid rounded card: any color other than kColorKey stays
        // fully visible under LWA_COLORKEY. The window region is clipped to
        // match this same rounded rect (see ApplyWindowShape).
        HBRUSH bgBrush = CreateSolidBrush(kColorBg);
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(memDC, bgBrush));
        HPEN nullPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
        HPEN oldPen = static_cast<HPEN>(SelectObject(memDC, nullPen));
        RoundRect(memDC, 0, 0, width, height, CORNER_RADIUS, CORNER_RADIUS);
        SelectObject(memDC, oldBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(bgBrush);
    } else {
        // Fully transparent: paint the whole card in the color-key color,
        // which SetLayeredWindowAttributes(..., LWA_COLORKEY) makes
        // invisible. Only the text/divider pixels drawn below stay opaque.
        HBRUSH keyBrush = CreateSolidBrush(kColorKey);
        FillRect(memDC, &rc, keyBrush);
        DeleteObject(keyBrush);
    }

    SetBkMode(memDC, TRANSPARENT);

    wchar_t ramLabel[48];
    swprintf_s(ramLabel, L"RAM %.1f/%.1fGB", g_ramUsedGB, g_ramTotalGB);

    if (g_horizontalMode) {
        int x = PAD;
        int y = PAD;
        x += DrawMetricInline(memDC, x, y, L"CPU", g_cpuPercent);
        x = DrawDivider(memDC, x, INLINE_ROW_H);
        x += DrawMetricInline(memDC, x, y, ramLabel, g_ramPercent);
        for (size_t i = 0; i < g_gpuStats.size(); ++i) {
            x = DrawDivider(memDC, x, INLINE_ROW_H);
            std::wstring gpuLabel = GpuRowLabel(i, g_gpuStats[i].name);
            x += DrawMetricInline(memDC, x, y, gpuLabel.c_str(), g_gpuStats[i].percent);
        }
    } else {
        int labelColW, valueColW;
        ComputeColumnWidths(memDC, labelColW, valueColW);
        int rowWidth = width - 2 * PAD;
        int barW = std::max(MIN_BAR_W, rowWidth - labelColW - valueColW - 2 * COL_GAP);
        int rowY = PAD;

        DrawRow(memDC, PAD, rowY, labelColW, barW, valueColW, L"CPU", g_cpuPercent, &g_cpuHistory);
        rowY += ROW_H;
        DrawRow(memDC, PAD, rowY, labelColW, barW, valueColW, ramLabel, g_ramPercent, &g_ramHistory);
        rowY += ROW_H;
        for (size_t i = 0; i < g_gpuStats.size(); ++i) {
            std::wstring gpuLabel = GpuRowLabel(i, g_gpuStats[i].name);
            DrawRow(memDC, PAD, rowY, labelColW, barW, valueColW, gpuLabel.c_str(), g_gpuStats[i].percent,
                    &g_gpuStats[i].history);
            rowY += ROW_H;
        }
    }

    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(hwnd, &ps);
}

// Computes the card's client size for whichever mode (vertical/horizontal)
// is currently active, from the actual measured text - shared by initial
// window creation and by the layout toggle so both stay in sync.
SIZE ComputeCardSize(HDC dc) {
    wchar_t ramLabel[48];
    swprintf_s(ramLabel, L"RAM %.1f/%.1fGB", g_ramUsedGB, g_ramTotalGB);
    int valueW = TextWidth(dc, g_fontValue, L"100%");

    if (g_horizontalMode) {
        auto blockWidth = [&](const std::wstring& label) {
            return TextWidth(dc, g_fontLabel, label) + ROW_GAP / 2 + valueW;
        };
        int dividerSpan = 2 * ITEM_GAP + DIVIDER_W;
        int totalW = blockWidth(L"CPU") + dividerSpan + blockWidth(ramLabel);
        for (size_t i = 0; i < g_gpuStats.size(); ++i) {
            totalW += dividerSpan + blockWidth(GpuRowLabel(i, g_gpuStats[i].name));
        }

        SIZE sz;
        sz.cx = std::max(WND_MIN_W, std::min(WND_MAX_W_HORIZONTAL, totalW + 2 * PAD));
        sz.cy = 2 * PAD + INLINE_ROW_H;
        return sz;
    }

    int labelColW, valueColW;
    ComputeColumnWidths(dc, labelColW, valueColW);
    int contentW = labelColW + COL_GAP + PREFERRED_BAR_W + COL_GAP + valueColW;

    SIZE sz;
    sz.cx = std::max(WND_MIN_W, std::min(WND_MAX_W_VERTICAL, contentW + 2 * PAD));
    sz.cy = PAD + ROW_H * (2 + static_cast<int>(g_gpuStats.size())) + PAD;
    return sz;
}

// Keeps a window of size (w,h) anchored at (x,y) fully within the monitor's
// work area (screen minus taskbar), nudging it back on-screen if it isn't.
void ClampToWorkArea(int& x, int& y, int w, int h) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (x + w > work.right) x = work.right - w;
    if (y + h > work.bottom) y = work.bottom - h;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
}

// The window is only clipped to a rounded region in solid-background mode -
// when transparent, there's no visible box to round, and clipping would
// just crop the floating text/bars for no reason.
void ApplyWindowShape(HWND hwnd, int w, int h) {
    if (g_showBg) {
        SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, CORNER_RADIUS * 2, CORNER_RADIUS * 2), TRUE);
    } else {
        SetWindowRgn(hwnd, nullptr, TRUE);
    }
}

void ReapplyWindowShape(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    ApplyWindowShape(hwnd, rc.right, rc.bottom);
}

// Resizes the window for the current layout mode, keeping its top-left
// corner where it was unless that would push the (often much wider or
// narrower) new size off-screen - then it's nudged back into the work area.
void RelayoutWindow(HWND hwnd) {
    HDC dc = GetDC(nullptr);
    SIZE sz = ComputeCardSize(dc);
    ReleaseDC(nullptr, dc);

    RECT wr{};
    GetWindowRect(hwnd, &wr);
    int x = wr.left;
    int y = wr.top;
    ClampToWorkArea(x, y, sz.cx, sz.cy);

    SetWindowPos(hwnd, nullptr, x, y, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    ApplyWindowShape(hwnd, sz.cx, sz.cy);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// --- settings persistence (HKCU\Software\PerfOverlay) -------------------

const wchar_t* kRegPath = L"Software\\PerfOverlay";

DWORD RegReadDword(HKEY key, const wchar_t* name, DWORD defaultVal) {
    DWORD val = defaultVal, size = sizeof(DWORD), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&val), &size) != ERROR_SUCCESS ||
        type != REG_DWORD) {
        return defaultVal;
    }
    return val;
}

void RegWriteDword(HKEY key, const wchar_t* name, DWORD val) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
}

// Loads saved toggle states and window position (if any) into the globals
// before the window is created. Leaves defaults untouched on first run.
void LoadSettings(int& x, int& y, bool& hasPosition) {
    hasPosition = false;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return;

    g_horizontalMode = RegReadDword(key, L"HorizontalMode", 0) != 0;
    g_showBg = RegReadDword(key, L"ShowBackground", 0) != 0;
    g_locked = RegReadDword(key, L"Locked", 0) != 0;
    g_hudVisible = RegReadDword(key, L"Visible", 1) != 0;
    g_showSparkline = RegReadDword(key, L"ShowSparkline", 1) != 0;
    g_hotkeyToggleEnabled = RegReadDword(key, L"HotkeyToggle", 0) != 0;
    g_hotkeyLockEnabled = RegReadDword(key, L"HotkeyLock", 0) != 0;
    g_autoHideFullscreen = RegReadDword(key, L"AutoHideFullscreen", 0) != 0;

    if (RegReadDword(key, L"HasPosition", 0) != 0) {
        x = static_cast<int>(RegReadDword(key, L"WindowX", 0));
        y = static_cast<int>(RegReadDword(key, L"WindowY", 0));
        hasPosition = true;
    }

    RegCloseKey(key);
}

void SaveSettings(HWND hwnd) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }

    RegWriteDword(key, L"HorizontalMode", g_horizontalMode ? 1 : 0);
    RegWriteDword(key, L"ShowBackground", g_showBg ? 1 : 0);
    RegWriteDword(key, L"Locked", g_locked ? 1 : 0);
    RegWriteDword(key, L"Visible", g_hudVisible ? 1 : 0);
    RegWriteDword(key, L"ShowSparkline", g_showSparkline ? 1 : 0);
    RegWriteDword(key, L"HotkeyToggle", g_hotkeyToggleEnabled ? 1 : 0);
    RegWriteDword(key, L"HotkeyLock", g_hotkeyLockEnabled ? 1 : 0);
    RegWriteDword(key, L"AutoHideFullscreen", g_autoHideFullscreen ? 1 : 0);

    RECT wr{};
    if (GetWindowRect(hwnd, &wr)) {
        RegWriteDword(key, L"WindowX", static_cast<DWORD>(wr.left));
        RegWriteDword(key, L"WindowY", static_cast<DWORD>(wr.top));
        RegWriteDword(key, L"HasPosition", 1);
    }

    RegCloseKey(key);
}

// --- auto-start (HKCU\...\Run) -------------------------------------------

const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValueName = L"PerfOverlay";

// Queried fresh each time the menu opens rather than cached, so it can never
// drift out of sync with the actual Run key (e.g. if edited elsewhere).
bool IsAutoStartEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    LONG res = RegQueryValueExW(key, kRunValueName, nullptr, &type, nullptr, nullptr);
    RegCloseKey(key);
    return res == ERROR_SUCCESS && type == REG_SZ;
}

void SetAutoStart(bool enable) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
            RegSetValueExW(key, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(quoted.c_str()),
                           static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(key, kRunValueName);
    }
    RegCloseKey(key);
}

// --- tray icon ------------------------------------------------------

void SetHudVisible(HWND hwnd, bool visible) {
    g_hudVisible = visible;
    ShowWindow(hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

// Locking adds WS_EX_TRANSPARENT on top of the existing WS_EX_LAYERED style,
// which makes Windows pass every click/drag straight through to whatever is
// underneath instead of hit-testing this window at all. While locked, the
// HUD itself can no longer be dragged or right-clicked - only the tray icon
// (a separate window) can unlock it again.
void SetLocked(HWND hwnd, bool locked) {
    g_locked = locked;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (locked) exStyle |= WS_EX_TRANSPARENT;
    else exStyle &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
}

// Standard heuristic used by several tray utilities: the foreground window
// isn't a desktop/shell window and its rect exactly covers a whole monitor.
// Cheap enough (a handful of API calls) to run on every 1s tick.
bool IsForegroundFullscreen(HWND selfHwnd) {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == selfHwnd) return false;

    wchar_t cls[256];
    if (GetClassNameW(fg, cls, 256) == 0) return false;
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
        return false;
    }

    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

// Hides the HUD while a fullscreen app is in front, without touching
// g_hudVisible (the user's actual choice) - so when the fullscreen app
// closes, the HUD only comes back if the user hadn't separately hidden it.
void UpdateAutoHide(HWND hwnd) {
    if (!g_autoHideFullscreen) {
        if (g_autoHiddenByFullscreen) {
            g_autoHiddenByFullscreen = false;
            if (g_hudVisible) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        return;
    }

    bool fullscreen = IsForegroundFullscreen(hwnd);
    if (fullscreen && !g_autoHiddenByFullscreen) {
        g_autoHiddenByFullscreen = true;
        ShowWindow(hwnd, SW_HIDE);
    } else if (!fullscreen && g_autoHiddenByFullscreen) {
        g_autoHiddenByFullscreen = false;
        if (g_hudVisible) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
}

void ShowActionMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_MENU_TOGGLE, g_hudVisible ? L"Hide" : L"Show");
    AppendMenuW(menu, MF_STRING, ID_MENU_LAYOUT, g_horizontalMode ? L"Vertical Layout" : L"Horizontal Layout");
    AppendMenuW(menu, MF_STRING, ID_MENU_BG, g_showBg ? L"Remove Background" : L"Add Background");
    AppendMenuW(menu, MF_STRING, ID_MENU_LOCK, g_locked ? L"Unlock Position" : L"Lock Position");
    AppendMenuW(menu, MF_STRING, ID_MENU_AUTOSTART,
                IsAutoStartEnabled() ? L"Disable Start with Windows" : L"Enable Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// --- settings window --------------------------------------------------
// Owner-drawn throughout (BS_OWNERDRAW instead of BS_AUTOCHECKBOX /
// BS_DEFPUSHBUTTON) so it can match the HUD's dark/monospace/cyan-accent
// look - native checkbox glyphs and button chrome ignore custom colors/
// fonts regardless of WM_CTLCOLORBTN, so there's no themeable middle ground
// short of full owner-draw.

constexpr int CHK_BG = 2001;
constexpr int CHK_HORIZONTAL = 2002;
constexpr int CHK_LOCK = 2003;
constexpr int CHK_SPARKLINE = 2004;
constexpr int CHK_AUTOSTART = 2005;
constexpr int CHK_HOTKEY_TOGGLE = 2006;
constexpr int CHK_HOTKEY_LOCK = 2007;
constexpr int CHK_AUTOHIDE_FULLSCREEN = 2008;
constexpr int BTN_CLOSE = 2009;

constexpr int SETTINGS_PAD = 20;
constexpr int SETTINGS_CHK_BOX = 14;
constexpr int SETTINGS_CHK_GAP = 10;
constexpr int SETTINGS_ROW_H = 30;
constexpr int SETTINGS_BTN_W = 100;
constexpr int SETTINGS_BTN_H = 30;
constexpr int SETTINGS_BTN_GAP = 18;

const wchar_t* kSettingsLabels[] = {
    L"Show background",
    L"Horizontal layout",
    L"Lock position (click-through)",
    L"Show history graphs (vertical layout only)",
    L"Start with Windows",
    L"Global hotkey Ctrl+Alt+O: Show/Hide",
    L"Global hotkey Ctrl+Alt+L: Lock/Unlock",
    L"Auto-hide when a game is fullscreen",
};
constexpr int kSettingsRowCount = 8;

// BM_SETCHECK/BM_GETCHECK only track state for the standard checkbox button
// styles (BS_CHECKBOX/BS_AUTOCHECKBOX/...) - BS_OWNERDRAW isn't one of them,
// so the check state is tracked by hand via GWLP_USERDATA instead.
bool IsChecked(HWND checkbox) {
    return GetWindowLongPtrW(checkbox, GWLP_USERDATA) != 0;
}

void SetChecked(HWND checkbox, bool checked) {
    SetWindowLongPtrW(checkbox, GWLP_USERDATA, checked ? 1 : 0);
}

// Sized to fit the longest label at g_fontLabel's actual measured width, so
// nothing gets clipped regardless of font/DPI - same principle as the HUD's
// own dynamic sizing.
SIZE ComputeSettingsClientSize(HDC dc) {
    int textW = 0;
    for (int i = 0; i < kSettingsRowCount; ++i) {
        textW = std::max(textW, TextWidth(dc, g_fontLabel, kSettingsLabels[i]));
    }
    SIZE sz;
    sz.cx = 2 * SETTINGS_PAD + SETTINGS_CHK_BOX + SETTINGS_CHK_GAP + textW;
    sz.cy = SETTINGS_PAD + SETTINGS_ROW_H * kSettingsRowCount + SETTINGS_BTN_GAP + SETTINGS_BTN_H + SETTINGS_PAD;
    return sz;
}

void DrawSettingsCheckbox(LPDRAWITEMSTRUCT dis) {
    HDC hdc = dis->hDC;
    const RECT& rc = dis->rcItem;
    bool checked = IsChecked(dis->hwndItem);

    HBRUSH bgBrush = CreateSolidBrush(kColorBg);
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    int boxTop = rc.top + (rc.bottom - rc.top - SETTINGS_CHK_BOX) / 2;
    int boxLeft = rc.left;

    HPEN boxPen = CreatePen(PS_SOLID, 1, checked ? kColorAccent : kColorTrack);
    HBRUSH boxBrush = CreateSolidBrush(checked ? kColorAccent : kColorBg);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, boxPen));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, boxBrush));
    Rectangle(hdc, boxLeft, boxTop, boxLeft + SETTINGS_CHK_BOX, boxTop + SETTINGS_CHK_BOX);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(boxPen);
    DeleteObject(boxBrush);

    if (checked) {
        HPEN checkPen = CreatePen(PS_SOLID, 2, kColorBg);
        HPEN oldCheckPen = static_cast<HPEN>(SelectObject(hdc, checkPen));
        MoveToEx(hdc, boxLeft + 3, boxTop + 7, nullptr);
        LineTo(hdc, boxLeft + 6, boxTop + 10);
        LineTo(hdc, boxLeft + 11, boxTop + 3);
        SelectObject(hdc, oldCheckPen);
        DeleteObject(checkPen);
    }

    wchar_t text[128];
    GetWindowTextW(dis->hwndItem, text, 128);
    SelectObject(hdc, g_fontLabel);
    SetTextColor(hdc, kColorLabelText);
    SetBkMode(hdc, TRANSPARENT);
    SIZE textSz{};
    GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &textSz);
    int textY = rc.top + (rc.bottom - rc.top - textSz.cy) / 2;
    TextOutW(hdc, boxLeft + SETTINGS_CHK_BOX + SETTINGS_CHK_GAP, textY, text, static_cast<int>(wcslen(text)));
}

void DrawSettingsButton(LPDRAWITEMSTRUCT dis) {
    HDC hdc = dis->hDC;
    const RECT& rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH brush = CreateSolidBrush(pressed ? kColorAccent : kColorTrack);
    HPEN pen = CreatePen(PS_SOLID, 1, kColorAccent);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, brush));
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t text[32];
    GetWindowTextW(dis->hwndItem, text, 32);
    SelectObject(hdc, g_fontLabel);
    SetTextColor(hdc, pressed ? kColorBg : kColorAccent);
    SetBkMode(hdc, TRANSPARENT);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
    TextOutW(hdc, rc.left + ((rc.right - rc.left) - sz.cx) / 2, rc.top + ((rc.bottom - rc.top) - sz.cy) / 2, text,
             static_cast<int>(wcslen(text)));
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

            RECT client{};
            GetClientRect(hwnd, &client);
            int rowW = client.right - 2 * SETTINGS_PAD;

            int id[] = {CHK_BG,          CHK_HORIZONTAL,     CHK_LOCK,        CHK_SPARKLINE,
                        CHK_AUTOSTART,   CHK_HOTKEY_TOGGLE,  CHK_HOTKEY_LOCK, CHK_AUTOHIDE_FULLSCREEN};
            bool checked[] = {g_showBg,
                               g_horizontalMode,
                               g_locked,
                               g_showSparkline,
                               IsAutoStartEnabled(),
                               g_hotkeyToggleEnabled,
                               g_hotkeyLockEnabled,
                               g_autoHideFullscreen};

            int y = SETTINGS_PAD;
            for (int i = 0; i < kSettingsRowCount; ++i) {
                HWND c = CreateWindowExW(0, L"BUTTON", kSettingsLabels[i], WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                          SETTINGS_PAD, y, rowW, SETTINGS_ROW_H, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id[i])), g_hInstance, nullptr);
                SetChecked(c, checked[i]);
                y += SETTINGS_ROW_H;
            }

            CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, SETTINGS_PAD,
                             y + SETTINGS_BTN_GAP, SETTINGS_BTN_W, SETTINGS_BTN_H, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(BTN_CLOSE)), g_hInstance, nullptr);
            return 0;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
            if (dis->CtlID == static_cast<UINT>(BTN_CLOSE)) DrawSettingsButton(dis);
            else DrawSettingsCheckbox(dis);
            return TRUE;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            if (id == BTN_CLOSE) {
                DestroyWindow(hwnd);
                return 0;
            }

            // BS_OWNERDRAW replaces BS_AUTOCHECKBOX's automatic toggle-on-click,
            // so the check state has to be flipped by hand before acting on it.
            HWND ctrl = reinterpret_cast<HWND>(lParam);
            bool newChecked = !IsChecked(ctrl);
            SetChecked(ctrl, newChecked);
            InvalidateRect(ctrl, nullptr, TRUE);

            switch (id) {
                case CHK_BG:
                    g_showBg = newChecked;
                    ReapplyWindowShape(g_mainHwnd);
                    InvalidateRect(g_mainHwnd, nullptr, TRUE);
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_HORIZONTAL:
                    g_horizontalMode = newChecked;
                    RelayoutWindow(g_mainHwnd);
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_LOCK:
                    SetLocked(g_mainHwnd, newChecked);
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_SPARKLINE:
                    g_showSparkline = newChecked;
                    RelayoutWindow(g_mainHwnd);
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_AUTOSTART:
                    SetAutoStart(newChecked);
                    break;
                case CHK_HOTKEY_TOGGLE:
                    g_hotkeyToggleEnabled = newChecked;
                    if (newChecked) {
                        RegisterHotKey(g_mainHwnd, HOTKEY_ID_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O');
                    } else {
                        UnregisterHotKey(g_mainHwnd, HOTKEY_ID_TOGGLE);
                    }
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_HOTKEY_LOCK:
                    g_hotkeyLockEnabled = newChecked;
                    if (newChecked) {
                        RegisterHotKey(g_mainHwnd, HOTKEY_ID_LOCK, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'L');
                    } else {
                        UnregisterHotKey(g_mainHwnd, HOTKEY_ID_LOCK);
                    }
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_AUTOHIDE_FULLSCREEN:
                    g_autoHideFullscreen = newChecked;
                    SaveSettings(g_mainHwnd);
                    break;
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_settingsHwnd = nullptr;
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Opens the settings window, or just refocuses it if already open - only
// one instance at a time.
void OpenSettingsWindow() {
    if (g_settingsHwnd && IsWindow(g_settingsHwnd)) {
        SetForegroundWindow(g_settingsHwnd);
        return;
    }

    static bool classRegistered = false;
    const wchar_t* className = L"PerfOverlaySettingsWndClass";
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = g_hInstance;
        wc.lpszClassName = className;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(kColorBg);
        wc.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCE(IDI_APPICON));
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    HDC measureDC = GetDC(nullptr);
    SIZE clientSz = ComputeSettingsClientSize(measureDC);
    ReleaseDC(nullptr, measureDC);

    RECT rc{0, 0, clientSz.cx, clientSz.cy};
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    DWORD exStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - w) / 2;
    int y = (screenH - h) / 2;

    g_settingsHwnd = CreateWindowExW(exStyle, className, L"PerfOverlay Settings", style, x, y, w, h, nullptr,
                                      nullptr, g_hInstance, nullptr);
}

// --- window procedure -----------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            InitCpuCounter();
            InitGpuQuery();
            RefreshGpuInstances();
            PdhCollectQueryData(g_gpuQuery); // prime first sample
            UpdateRam();
            SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL_MS, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_ID) {
                ++g_tickCount;
                UpdateCpu();
                UpdateRam();
                if (g_tickCount % 5 == 0) RefreshGpuInstances();
                UpdateGpu();

                PushHistory(g_cpuHistory, g_cpuPercent);
                PushHistory(g_ramHistory, g_ramPercent);
                for (auto& stat : g_gpuStats) PushHistory(stat.history, stat.percent);

                UpdateAutoHide(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            OnPaint(hwnd);
            return 0;

        case WM_LBUTTONDOWN:
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;

        case WM_EXITSIZEMOVE: // drag finished
            SaveSettings(hwnd);
            return 0;

        case WM_HOTKEY:
            if (wParam == HOTKEY_ID_TOGGLE) {
                SetHudVisible(hwnd, !g_hudVisible);
                SaveSettings(hwnd);
            } else if (wParam == HOTKEY_ID_LOCK) {
                SetLocked(hwnd, !g_locked);
                SaveSettings(hwnd);
            }
            return 0;

        case WM_RBUTTONUP:
            ShowActionMenu(hwnd);
            return 0;

        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP) ShowActionMenu(hwnd);
            else if (lParam == WM_LBUTTONUP) { // quick toggle
                SetHudVisible(hwnd, !g_hudVisible);
                SaveSettings(hwnd);
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_MENU_EXIT) DestroyWindow(hwnd);
            else if (LOWORD(wParam) == ID_MENU_TOGGLE) {
                SetHudVisible(hwnd, !g_hudVisible);
                SaveSettings(hwnd);
            } else if (LOWORD(wParam) == ID_MENU_LAYOUT) {
                g_horizontalMode = !g_horizontalMode;
                RelayoutWindow(hwnd);
                SaveSettings(hwnd);
            } else if (LOWORD(wParam) == ID_MENU_BG) {
                g_showBg = !g_showBg;
                ReapplyWindowShape(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
                SaveSettings(hwnd);
            } else if (LOWORD(wParam) == ID_MENU_LOCK) {
                SetLocked(hwnd, !g_locked);
                SaveSettings(hwnd);
            } else if (LOWORD(wParam) == ID_MENU_AUTOSTART) {
                SetAutoStart(!IsAutoStartEnabled());
            } else if (LOWORD(wParam) == ID_MENU_SETTINGS) {
                OpenSettingsWindow();
            }
            return 0;

        case WM_DESTROY:
            SaveSettings(hwnd);
            UnregisterHotKey(hwnd, HOTKEY_ID_TOGGLE);
            UnregisterHotKey(hwnd, HOTKEY_ID_LOCK);
            KillTimer(hwnd, TIMER_ID);
            if (g_cpuQuery) PdhCloseQuery(g_cpuQuery);
            if (g_gpuQuery) PdhCloseQuery(g_gpuQuery);
            if (g_trayIconAdded) Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
            if (g_settingsHwnd) DestroyWindow(g_settingsHwnd);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();
    g_hInstance = hInstance;

    g_gpuStats = EnumerateGpuAdapters();
    if (g_gpuStats.empty()) {
        GpuStat fallback;
        fallback.luid = 0;
        fallback.name = L"GPU";
        fallback.percent = 0.0;
        g_gpuStats.push_back(fallback);
        g_gpuSingleFallback = true;
    }
    UpdateRam(); // real GB figures for the width measurement below

    int savedX = 0, savedY = 0;
    bool hasSavedPosition = false;
    LoadSettings(savedX, savedY, hasSavedPosition); // sets g_horizontalMode/g_showBg/g_locked/g_hudVisible

    // ANTIALIASED_QUALITY (not CLEARTYPE_QUALITY): ClearType's subpixel
    // blending assumes a known solid background, which produces color
    // fringing once the "background" is actually a color-key transparent
    // hole showing an arbitrary desktop through it. Plain grayscale AA
    // avoids that fringe. Monospace (Consolas) for the dashboard look.
    g_fontLabel = CreateFontW(
        -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    g_fontValue = CreateFontW(
        -14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    // Size the card to fit the widest content (GPU names in particular)
    // instead of truncating text to a fixed window width.
    HDC measureDC = GetDC(nullptr);
    SIZE cardSize = ComputeCardSize(measureDC);
    ReleaseDC(nullptr, measureDC);
    int wndW = cardSize.cx;
    int wndH = cardSize.cy;

    const wchar_t* className = L"PerfOverlayWndClass";

    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_APPICON));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = hIcon;
    wc.hIconSm = hIcon;
    RegisterClassExW(&wc);

    int x, y;
    if (hasSavedPosition) {
        x = savedX;
        y = savedY;
        ClampToWorkArea(x, y, wndW, wndH); // in case resolution/monitor changed since last run
    } else {
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        x = screenW - wndW - 20;
        y = 20;
    }

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (g_locked) exStyle |= WS_EX_TRANSPARENT;

    HWND hwnd = CreateWindowExW(
        exStyle,
        className, L"PerfOverlay",
        WS_POPUP,
        x, y, wndW, wndH,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) return 0;
    g_mainHwnd = hwnd;

    SetLayeredWindowAttributes(hwnd, kColorKey, 0, LWA_COLORKEY);
    ApplyWindowShape(hwnd, wndW, wndH);

    if (g_hotkeyToggleEnabled) RegisterHotKey(hwnd, HOTKEY_ID_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O');
    if (g_hotkeyLockEnabled) RegisterHotKey(hwnd, HOTKEY_ID_LOCK, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'L');

    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = TRAY_ICON_ID;
    g_trayIcon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = hIcon;
    wcscpy_s(g_trayIcon.szTip, L"PerfOverlay");
    g_trayIconAdded = Shell_NotifyIconW(NIM_ADD, &g_trayIcon) != FALSE;

    SetHudVisible(hwnd, g_hudVisible);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_fontLabel) DeleteObject(g_fontLabel);
    if (g_fontValue) DeleteObject(g_fontValue);
    return static_cast<int>(msg.wParam);
}
