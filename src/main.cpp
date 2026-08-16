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
bool g_showSparkline = false;  // vertical mode only: small history graph per row
bool g_hotkeyToggleEnabled = false;
bool g_hotkeyLockEnabled = false;
bool g_autoHideFullscreen = false; // hide the HUD while a game/app is exclusive-fullscreen
bool g_autoHiddenByFullscreen = false; // true while we (not the user) are hiding it

HWND g_mainHwnd = nullptr;
HWND g_settingsHwnd = nullptr;
HINSTANCE g_hInstance = nullptr;

// Layout constants for the transparent, borderless HUD: no box, no swatch -
// just "label value" pairs, separated by thin divider lines.
constexpr int PAD = 8;
constexpr int ROW_H = 24;              // vertical mode: row-to-row spacing
constexpr int INLINE_ROW_H = 18;       // horizontal mode: single line content height
constexpr int ROW_GAP = 10;            // gap between label and value
constexpr int ITEM_GAP = 14;           // horizontal mode: padding either side of a divider
constexpr int DIVIDER_W = 1;
constexpr int WND_MIN_W = 140;
constexpr int WND_MAX_W_VERTICAL = 380;
constexpr int WND_MAX_W_HORIZONTAL = 640;
constexpr int SPARK_W = 36;            // vertical mode: history graph width
constexpr int SPARK_H = 12;
constexpr size_t HISTORY_LEN = 40;     // ~40s of samples at 1 tick/s

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
constexpr COLORREF kColorBg = RGB(17, 17, 21);
constexpr COLORREF kColorCardBorder = RGB(48, 48, 55);
constexpr COLORREF kColorDivider = RGB(110, 110, 122);
constexpr COLORREF kColorLabelText = RGB(210, 210, 218);

COLORREF ColorForLoad(double percent) {
    if (percent < 50.0) return RGB(94, 222, 138);
    if (percent < 80.0) return RGB(244, 196, 66);
    return RGB(244, 84, 84);
}

int TextWidth(HDC hdc, HFONT font, const std::wstring& text) {
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
    SIZE sz{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(hdc, old);
    return sz.cx;
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

// Draws one "label value%" pair, label left, value right-aligned within
// `width`, with an optional sparkline squeezed in between. No swatch, no
// box - just text (and the graph, when enabled).
void DrawRow(HDC hdc, int x, int y, int width, const wchar_t* label, double percent,
             const std::vector<float>* history) {
    percent = std::max(0.0, std::min(100.0, percent));
    COLORREF color = ColorForLoad(percent);

    SelectObject(hdc, g_fontLabel);
    SetTextColor(hdc, kColorLabelText);
    TextOutW(hdc, x, y, label, static_cast<int>(wcslen(label)));

    wchar_t valBuf[16];
    swprintf_s(valBuf, L"%.0f%%", percent);
    SelectObject(hdc, g_fontValue);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, valBuf, static_cast<int>(wcslen(valBuf)), &sz);
    int valueX = x + width - sz.cx;

    if (g_showSparkline && history) {
        int sparkX = valueX - ROW_GAP / 2 - SPARK_W;
        int sparkY = y + 2;
        DrawSparkline(hdc, sparkX, sparkY, SPARK_W, SPARK_H, *history, color);
    }

    SetTextColor(hdc, color);
    TextOutW(hdc, valueX, y - 3, valBuf, static_cast<int>(wcslen(valBuf)));
}

// Horizontal-mode variant: "label value%" packed tightly left-to-right (no
// right-alignment against a row width). Returns the pixel width used, so the
// caller can advance x for the next metric block.
int DrawMetricInline(HDC hdc, int x, int y, const wchar_t* label, double percent) {
    percent = std::max(0.0, std::min(100.0, percent));
    COLORREF color = ColorForLoad(percent);

    SelectObject(hdc, g_fontLabel);
    SIZE labelSz{};
    GetTextExtentPoint32W(hdc, label, static_cast<int>(wcslen(label)), &labelSz);
    SetTextColor(hdc, kColorLabelText);
    TextOutW(hdc, x, y, label, static_cast<int>(wcslen(label)));

    int valueX = x + labelSz.cx + ROW_GAP / 2;
    wchar_t valBuf[16];
    swprintf_s(valBuf, L"%.0f%%", percent);
    SelectObject(hdc, g_fontValue);
    SIZE valSz{};
    GetTextExtentPoint32W(hdc, valBuf, static_cast<int>(wcslen(valBuf)), &valSz);
    SetTextColor(hdc, color);
    TextOutW(hdc, valueX, y - 3, valBuf, static_cast<int>(wcslen(valBuf)));

    return (valueX + valSz.cx) - x;
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
        // Opaque solid card: any color other than kColorKey stays fully
        // visible under LWA_COLORKEY, so this is a normal background fill.
        HPEN borderPen = CreatePen(PS_SOLID, 1, kColorCardBorder);
        HPEN oldPen = static_cast<HPEN>(SelectObject(memDC, borderPen));
        HBRUSH bgBrush = CreateSolidBrush(kColorBg);
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(memDC, bgBrush));
        Rectangle(memDC, 0, 0, width, height);
        SelectObject(memDC, oldBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(bgBrush);
        DeleteObject(borderPen);
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
        for (auto& stat : g_gpuStats) {
            x = DrawDivider(memDC, x, INLINE_ROW_H);
            x += DrawMetricInline(memDC, x, y, stat.name.c_str(), stat.percent);
        }
    } else {
        int rowX = PAD;
        int rowWidth = width - 2 * PAD;
        int rowY = PAD;

        DrawRow(memDC, rowX, rowY, rowWidth, L"CPU", g_cpuPercent, &g_cpuHistory);
        rowY += ROW_H;
        DrawRow(memDC, rowX, rowY, rowWidth, ramLabel, g_ramPercent, &g_ramHistory);
        rowY += ROW_H;
        for (auto& stat : g_gpuStats) {
            DrawRow(memDC, rowX, rowY, rowWidth, stat.name.c_str(), stat.percent, &stat.history);
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
        for (auto& stat : g_gpuStats) totalW += dividerSpan + blockWidth(stat.name);

        SIZE sz;
        sz.cx = std::max(WND_MIN_W, std::min(WND_MAX_W_HORIZONTAL, totalW + 2 * PAD));
        sz.cy = 2 * PAD + INLINE_ROW_H;
        return sz;
    }

    int sparkExtra = g_showSparkline ? (SPARK_W + ROW_GAP) : 0;
    int contentW = std::max(TextWidth(dc, g_fontLabel, L"CPU") + ROW_GAP + sparkExtra + valueW,
                             TextWidth(dc, g_fontLabel, ramLabel) + ROW_GAP + sparkExtra + valueW);
    for (auto& stat : g_gpuStats) {
        contentW = std::max(contentW, TextWidth(dc, g_fontLabel, stat.name) + ROW_GAP + sparkExtra + valueW);
    }

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
    g_showSparkline = RegReadDword(key, L"ShowSparkline", 0) != 0;
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

constexpr int CHK_BG = 2001;
constexpr int CHK_HORIZONTAL = 2002;
constexpr int CHK_LOCK = 2003;
constexpr int CHK_SPARKLINE = 2004;
constexpr int CHK_AUTOSTART = 2005;
constexpr int CHK_HOTKEY_TOGGLE = 2006;
constexpr int CHK_HOTKEY_LOCK = 2007;
constexpr int CHK_AUTOHIDE_FULLSCREEN = 2008;
constexpr int BTN_CLOSE = 2009;

bool IsChecked(HWND checkbox) {
    return SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT uiFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            int y = 16;
            auto addCheck = [&](int id, const wchar_t* text, bool checked) {
                HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                          16, y, 320, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                          g_hInstance, nullptr);
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);
                SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
                y += 28;
            };

            addCheck(CHK_BG, L"Show background", g_showBg);
            addCheck(CHK_HORIZONTAL, L"Horizontal layout", g_horizontalMode);
            addCheck(CHK_LOCK, L"Lock position (click-through)", g_locked);
            addCheck(CHK_SPARKLINE, L"Show history graphs (vertical layout only)", g_showSparkline);
            addCheck(CHK_AUTOSTART, L"Start with Windows", IsAutoStartEnabled());
            addCheck(CHK_HOTKEY_TOGGLE, L"Global hotkey Ctrl+Alt+O: Show/Hide", g_hotkeyToggleEnabled);
            addCheck(CHK_HOTKEY_LOCK, L"Global hotkey Ctrl+Alt+L: Lock/Unlock", g_hotkeyLockEnabled);
            addCheck(CHK_AUTOHIDE_FULLSCREEN, L"Auto-hide when a game is fullscreen", g_autoHideFullscreen);

            HWND closeBtn = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 16,
                                             y + 8, 90, 26, hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(BTN_CLOSE)), g_hInstance,
                                             nullptr);
            SendMessageW(closeBtn, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            HWND ctrl = reinterpret_cast<HWND>(lParam);

            switch (id) {
                case CHK_BG:
                    g_showBg = IsChecked(ctrl);
                    InvalidateRect(g_mainHwnd, nullptr, TRUE);
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_HORIZONTAL:
                    g_horizontalMode = IsChecked(ctrl);
                    RelayoutWindow(g_mainHwnd);
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_LOCK:
                    SetLocked(g_mainHwnd, IsChecked(ctrl));
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_SPARKLINE:
                    g_showSparkline = IsChecked(ctrl);
                    RelayoutWindow(g_mainHwnd);
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_AUTOSTART:
                    SetAutoStart(IsChecked(ctrl));
                    break;
                case CHK_HOTKEY_TOGGLE:
                    g_hotkeyToggleEnabled = IsChecked(ctrl);
                    if (g_hotkeyToggleEnabled) {
                        RegisterHotKey(g_mainHwnd, HOTKEY_ID_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O');
                    } else {
                        UnregisterHotKey(g_mainHwnd, HOTKEY_ID_TOGGLE);
                    }
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_HOTKEY_LOCK:
                    g_hotkeyLockEnabled = IsChecked(ctrl);
                    if (g_hotkeyLockEnabled) {
                        RegisterHotKey(g_mainHwnd, HOTKEY_ID_LOCK, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'L');
                    } else {
                        UnregisterHotKey(g_mainHwnd, HOTKEY_ID_LOCK);
                    }
                    SaveSettings(g_mainHwnd);
                    break;
                case CHK_AUTOHIDE_FULLSCREEN:
                    g_autoHideFullscreen = IsChecked(ctrl);
                    SaveSettings(g_mainHwnd);
                    break;
                case BTN_CLOSE:
                    DestroyWindow(hwnd);
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
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCE(IDI_APPICON));
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    RECT rc{0, 0, 360, 268};
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
    // avoids that fringe.
    g_fontLabel = CreateFontW(
        -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_fontValue = CreateFontW(
        -17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

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
