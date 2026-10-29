/*
 * Leapdesk KVM -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Leapdesk KVM contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "platform/MSWindowsPasteProgress.h"

#include "common/win32/encoding_utilities.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <iterator>

namespace inputleap {

namespace {

const wchar_t kWindowClass[] = L"LeapdeskPasteProgress";
// the program's icon: IDI_INPUTLEAP in the server's and the client's resources
const int kIcon = 101;
const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
const DWORD kExStyle = WS_EX_TOPMOST;

// posted when there are updates to show, and to close the window
const UINT kWake = WM_APP + 1;
const UINT kClose = WM_APP + 2;
const UINT_PTR kShowTimer = 1;

// "...", as one character
const std::wstring kEllipsis(1, static_cast<wchar_t>(0x2026));

} // namespace

MSWindowsPasteProgress::MSWindowsPasteProgress(Cancel cancel, std::chrono::milliseconds delay) :
    cancel_(std::move(cancel)),
    delay_(delay)
{
    HANDLE created = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this, created]() { run(created); });
    WaitForSingleObject(created, INFINITE);
    CloseHandle(created);
}

MSWindowsPasteProgress::~MSWindowsPasteProgress()
{
    if (window_ != nullptr) {
        PostMessageW(window_, kClose, 0, 0);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MSWindowsPasteProgress::update(const Progress& progress)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        updates_.push_back(progress);
    }
    if (window_ != nullptr) {
        PostMessageW(window_, kWake, 0, 0);
    }
}

std::wstring MSWindowsPasteProgress::format_size(std::uint64_t bytes)
{
    if (bytes < 1024) {
        return std::to_wstring(bytes) + (bytes == 1 ? L" byte" : L" bytes");
    }
    static const wchar_t* const units[] = {L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes) / 1024;
    std::size_t unit = 0;
    while (value >= 1000 && unit + 1 < std::size(units)) {
        value /= 1024;
        ++unit;
    }
    // three figures, cut rather than rounded, as Explorer does
    int decimals = value < 10 ? 2 : (value < 100 ? 1 : 0);
    double scale = decimals == 2 ? 100 : (decimals == 1 ? 10 : 1);
    value = std::floor(value * scale) / scale;
    wchar_t text[32];
    std::swprintf(text, std::size(text), L"%.*f %ls", decimals, value, units[unit]);
    return text;
}

std::wstring MSWindowsPasteProgress::format_time_left(std::chrono::seconds left)
{
    long long seconds = (std::max)(left.count(), 1LL);
    if (seconds < 60) {
        // to the next five seconds, so it doesn't flicker
        return L"about " + std::to_wstring((seconds + 4) / 5 * 5) + L" seconds";
    }
    long long minutes = (seconds + 30) / 60;
    if (minutes < 60) {
        return L"about " + std::to_wstring(minutes) + (minutes == 1 ? L" minute" : L" minutes");
    }
    long long hours = minutes / 60;
    minutes %= 60;
    std::wstring text = L"about " + std::to_wstring(hours) + (hours == 1 ? L" hour" : L" hours");
    if (minutes != 0) {
        text += L" " + std::to_wstring(minutes) + (minutes == 1 ? L" minute" : L" minutes");
    }
    return text;
}

void MSWindowsPasteProgress::run(HANDLE created)
{
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &MSWindowsPasteProgress::window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kIcon));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kIcon),
                                                         IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXSMICON),
                                                         GetSystemMetrics(SM_CYSMICON), 0));
    // a second window in the same process finds the class already there
    RegisterClassExW(&window_class);

    HWND window = CreateWindowExW(kExStyle, kWindowClass, L"Leapdesk", kStyle, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this);
    if (window == nullptr) {
        SetEvent(created);
        return;
    }
    auto make = [&](const wchar_t* control_class, DWORD style, int id, const wchar_t* text) {
        CreateWindowExW(0, control_class, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    const DWORD label = SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS;
    make(L"STATIC", label, kHeading, L"");
    make(PROGRESS_CLASSW, 0, kBar, nullptr);
    make(L"STATIC", label, kAmount, L"");
    make(L"STATIC", label, kSpeed, L"");
    make(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, kCancel, L"Cancel");
    SendMessageW(GetDlgItem(window, kBar), PBM_SETRANGE32, 0, 1000);

    window_ = window;
    UINT dpi = GetDpiForWindow(window_);
    layout(dpi != 0 ? dpi : 96);
    SetEvent(created);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Esc and Tab as in a dialog
        if (!IsDialogMessageW(window_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

LRESULT CALLBACK MSWindowsPasteProgress::window_proc(HWND window, UINT message, WPARAM wparam,
                                                     LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto self = reinterpret_cast<MSWindowsPasteProgress*>(GetWindowLongPtrW(window,
                                                                            GWLP_USERDATA));
    // the window is set up once it and its controls have been made
    if (self != nullptr && self->window_ == window) {
        return self->handle(message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT MSWindowsPasteProgress::handle(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case kWake: {
        std::deque<Progress> updates;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            updates.swap(updates_);
        }
        for (const auto& progress : updates) {
            apply(progress);
        }
        return 0;
    }

    case WM_TIMER:
        if (wparam == kShowTimer) {
            KillTimer(window_, kShowTimer);
            if (!request_.empty()) {
                show();
            }
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wparam) == kCancel) {
            cancel();
        }
        return 0;

    case WM_CLOSE:
        // closing it calls the paste off, as with Explorer's own copy window
        cancel();
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        layout(HIWORD(wparam));
        return 0;
    }

    case kClose:
        DestroyWindow(window_);
        return 0;

    case WM_DESTROY:
        if (font_ != nullptr) {
            DeleteObject(font_);
        }
        if (heading_font_ != nullptr) {
            DeleteObject(heading_font_);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void MSWindowsPasteProgress::apply(const Progress& progress)
{
    using Stage = Progress::Stage;
    bool ended = progress.stage == Stage::DONE || progress.stage == Stage::FAILED;
    if (progress.request != request_) {
        if (ended) {
            return;
        }
        // a paste starts: the window shows if it's still going in a moment
        request_ = progress.request;
        cancelling_ = false;
        samples_.clear();
        EnableWindow(GetDlgItem(window_, kCancel), TRUE);
        SetTimer(window_, kShowTimer, static_cast<UINT>(delay_.count()), nullptr);
    }
    if (ended) {
        KillTimer(window_, kShowTimer);
        ShowWindow(window_, SW_HIDE);
        shown_ = false;
        request_.clear();
        return;
    }

    std::wstring them = progress.files == 1 ? L"it" : L"them";
    set_text(kHeading, progress.files == 1 ?
                           L"Pasting " + std::wstring(utf8_to_win_char(progress.name).data()) :
                           L"Pasting " + std::to_wstring(progress.files) + L" files");

    std::wstring amount;
    std::wstring speed;
    if (progress.stage == Stage::WAITING || !progress.counting) {
        // a sender running an older version doesn't say how far it has got
        set_marquee(true);
        if (progress.total > 0) {
            amount = format_size(progress.total);
        }
        speed = progress.stage == Stage::WAITING ?
                    L"Waiting for the other screen to send " + them + kEllipsis :
                    L"The other screen is sending " + them + kEllipsis;
    }
    else {
        std::uint64_t total = (std::max)(progress.total, progress.sent);
        double done = total == 0 ? 1.0 : static_cast<double>(progress.sent) / total;
        set_marquee(false);
        SendMessageW(GetDlgItem(window_, kBar), PBM_SETPOS, static_cast<WPARAM>(done * 1000), 0);
        amount = format_size(progress.sent) + L" of " + format_size(total) + L" (" +
                 std::to_wstring(static_cast<int>(done * 100)) + L"%)";

        if (progress.sent >= total) {
            speed = L"Finishing" + kEllipsis;
        }
        else {
            // the speed over the last few seconds
            auto now = std::chrono::steady_clock::now();
            samples_.emplace_back(now, progress.sent);
            while (samples_.size() > 2 && now - samples_[1].first >= std::chrono::seconds(5)) {
                samples_.pop_front();
            }
            auto elapsed = now - samples_.front().first;
            std::uint64_t moved = progress.sent - (std::min)(progress.sent,
                                                             samples_.front().second);
            if (elapsed >= std::chrono::seconds(1) && moved > 0) {
                double rate = moved / std::chrono::duration<double>(elapsed).count();
                auto left = std::chrono::seconds(
                    static_cast<long long>((total - progress.sent) / rate) + 1);
                speed = format_size(static_cast<std::uint64_t>(rate)) + L"/s, " +
                        format_time_left(left) + L" left";
            }
            else {
                speed = L"Working out the time left" + kEllipsis;
            }
        }
    }
    set_text(kAmount, amount);
    if (!cancelling_) {
        set_text(kSpeed, speed);
    }
}

void MSWindowsPasteProgress::show()
{
    // on the screen with the pointer, where the paste happened
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT& work = monitor.rcWork;

    // moved there first, so that it takes on that screen's scale
    SetWindowPos(window_, nullptr, work.left, work.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    UINT dpi = GetDpiForWindow(window_);
    layout(dpi != 0 ? dpi : 96);
    RECT frame{};
    GetWindowRect(window_, &frame);
    int width = frame.right - frame.left;
    int height = frame.bottom - frame.top;
    SetWindowPos(window_, HWND_TOPMOST, work.left + (work.right - work.left - width) / 2,
                 work.top + (work.bottom - work.top - height) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    shown_ = true;
}

void MSWindowsPasteProgress::cancel()
{
    if (request_.empty() || cancelling_) {
        return;
    }
    cancelling_ = true;
    EnableWindow(GetDlgItem(window_, kCancel), FALSE);
    set_text(kSpeed, L"Cancelling" + kEllipsis);
    if (cancel_) {
        cancel_(request_);
    }
}

void MSWindowsPasteProgress::layout(UINT dpi)
{
    auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW bold = metrics.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    HFONT heading_font = CreateFontIndirectW(&bold);

    // a line fits the text even when it has been made larger
    int line = (std::max)(scale(20),
                          static_cast<int>(std::abs(metrics.lfMessageFont.lfHeight)) * 3 / 2);
    int margin = scale(16);
    int width = scale(440);
    int inner = width - 2 * margin;
    int y = scale(14);
    auto place = [&](int id, int x, int top, int w, int h) {
        HWND control = GetDlgItem(window_, id);
        SetWindowPos(control, nullptr, x, top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(id == kHeading ? heading_font : font), TRUE);
    };
    place(kHeading, margin, y, inner, line);
    y += line + scale(8);
    place(kBar, margin, y, inner, scale(16));
    y += scale(16) + scale(8);
    place(kAmount, margin, y, inner, line);
    y += line + scale(2);
    place(kSpeed, margin, y, inner, line);
    y += line + scale(12);
    int button_width = scale(96);
    int button_height = (std::max)(scale(28), line + scale(8));
    place(kCancel, width - margin - button_width, y, button_width, button_height);
    y += button_height + scale(14);

    if (font_ != nullptr) {
        DeleteObject(font_);
    }
    if (heading_font_ != nullptr) {
        DeleteObject(heading_font_);
    }
    font_ = font;
    heading_font_ = heading_font;

    RECT frame{0, 0, width, y};
    AdjustWindowRectExForDpi(&frame, kStyle, FALSE, kExStyle, dpi);
    SetWindowPos(window_, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void MSWindowsPasteProgress::set_text(int control, const std::wstring& text)
{
    // only when it changes, so that it doesn't flicker
    HWND item = GetDlgItem(window_, control);
    wchar_t current[512] = {};
    GetWindowTextW(item, current, static_cast<int>(std::size(current)));
    if (text != current) {
        SetWindowTextW(item, text.c_str());
    }
}

void MSWindowsPasteProgress::set_marquee(bool marquee)
{
    if (marquee == marquee_) {
        return;
    }
    marquee_ = marquee;
    HWND bar = GetDlgItem(window_, kBar);
    LONG_PTR style = GetWindowLongPtrW(bar, GWL_STYLE);
    SetWindowLongPtrW(bar, GWL_STYLE, marquee ? (style | PBS_MARQUEE) : (style & ~PBS_MARQUEE));
    SendMessageW(bar, PBM_SETMARQUEE, marquee ? TRUE : FALSE, 30);
}

} // namespace inputleap
