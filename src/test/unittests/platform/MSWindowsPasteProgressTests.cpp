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

#include <gtest/gtest.h>

#include <commctrl.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace inputleap {

namespace {

using Progress = MSWindowsFileFetch::Progress;
using Stage = Progress::Stage;

const std::uint64_t kMB = 1024 * 1024;
const std::wstring kEllipsis(1, static_cast<wchar_t>(0x2026));

// a paste of a 400 MB file
Progress paste(const std::string& request, Stage stage, std::uint64_t sent = 0,
               bool counting = false)
{
    Progress progress;
    progress.request = request;
    progress.stage = stage;
    progress.files = 1;
    progress.name = "report.pptx";
    progress.total = 400 * kMB;
    progress.sent = sent;
    progress.counting = counting;
    return progress;
}

bool in_private_window_station()
{
    wchar_t name[256] = {};
    DWORD needed = 0;
    GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, name, sizeof(name), &needed);
    return _wcsicmp(name, L"WinSta0") != 0;
}

std::wstring text_of(HWND window, int control)
{
    wchar_t text[512] = {};
    GetWindowTextW(GetDlgItem(window, control), text, 512);
    return text;
}

// the window updates on a thread of its own, so its state is waited for
bool eventually(const std::function<bool()>& condition)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        Sleep(20);
    }
    return true;
}

void click(HWND window, int control)
{
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(control, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(window, control)));
}

} // namespace

TEST(MSWindowsPasteProgressTests, sizesReadAsExplorerShowsThem)
{
    EXPECT_EQ(L"0 bytes", MSWindowsPasteProgress::format_size(0));
    EXPECT_EQ(L"1 byte", MSWindowsPasteProgress::format_size(1));
    EXPECT_EQ(L"1023 bytes", MSWindowsPasteProgress::format_size(1023));
    EXPECT_EQ(L"1.00 KB", MSWindowsPasteProgress::format_size(1024));
    EXPECT_EQ(L"820 KB", MSWindowsPasteProgress::format_size(820 * 1024));
    EXPECT_EQ(L"0.97 MB", MSWindowsPasteProgress::format_size(1000 * 1024));
    EXPECT_EQ(L"12.3 MB", MSWindowsPasteProgress::format_size(12 * kMB + 350 * 1024));
    EXPECT_EQ(L"416 MB", MSWindowsPasteProgress::format_size(416 * kMB));
    // cut, not rounded up
    EXPECT_EQ(L"2.84 MB", MSWindowsPasteProgress::format_size(2988441));
    EXPECT_EQ(L"1.50 GB", MSWindowsPasteProgress::format_size(1536 * kMB));
}

TEST(MSWindowsPasteProgressTests, timeLeftIsRoughAndSteady)
{
    using std::chrono::seconds;
    EXPECT_EQ(L"about 5 seconds", MSWindowsPasteProgress::format_time_left(seconds(0)));
    EXPECT_EQ(L"about 5 seconds", MSWindowsPasteProgress::format_time_left(seconds(1)));
    EXPECT_EQ(L"about 10 seconds", MSWindowsPasteProgress::format_time_left(seconds(7)));
    EXPECT_EQ(L"about 60 seconds", MSWindowsPasteProgress::format_time_left(seconds(59)));
    EXPECT_EQ(L"about 1 minute", MSWindowsPasteProgress::format_time_left(seconds(60)));
    EXPECT_EQ(L"about 1 minute", MSWindowsPasteProgress::format_time_left(seconds(89)));
    EXPECT_EQ(L"about 2 minutes", MSWindowsPasteProgress::format_time_left(seconds(90)));
    EXPECT_EQ(L"about 1 hour", MSWindowsPasteProgress::format_time_left(seconds(3599)));
    EXPECT_EQ(L"about 1 hour 5 minutes", MSWindowsPasteProgress::format_time_left(seconds(3900)));
    EXPECT_EQ(L"about 2 hours 1 minute", MSWindowsPasteProgress::format_time_left(seconds(7260)));
}

class MSWindowsPasteProgressWindowTests : public ::testing::Test {
protected:
    void SetUp() override
    {
        // the window would show on the desktop of whoever runs the tests
        if (!in_private_window_station()) {
            GTEST_SKIP() << "shows a window; run in a window station of its own";
        }
    }
};

TEST_F(MSWindowsPasteProgressWindowTests, quickPasteShowsNothing)
{
    MSWindowsPasteProgress progress([](const std::string&) {}, std::chrono::milliseconds(300));
    ASSERT_NE(nullptr, progress.window());
    progress.update(paste("00000000000000a1", Stage::WAITING));
    progress.update(paste("00000000000000a1", Stage::DONE));
    Sleep(700);
    EXPECT_FALSE(progress.shown());
}

TEST_F(MSWindowsPasteProgressWindowTests, showsHowFarAPasteHasGotAndCancelsIt)
{
    std::mutex mutex;
    std::vector<std::string> cancelled;
    MSWindowsPasteProgress progress([&](const std::string& request) {
        std::lock_guard<std::mutex> lock(mutex);
        cancelled.push_back(request);
    }, std::chrono::milliseconds(200));
    HWND window = progress.window();
    ASSERT_NE(nullptr, window);
    auto visible = [&progress]() { return progress.shown(); };
    auto cancels = [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return cancelled;
    };

    // shown once the paste has taken a moment
    progress.update(paste("00000000000000a1", Stage::WAITING));
    ASSERT_TRUE(eventually(visible));
    EXPECT_EQ(L"Pasting report.pptx", text_of(window, MSWindowsPasteProgress::kHeading));
    EXPECT_EQ(L"400 MB", text_of(window, MSWindowsPasteProgress::kAmount));
    EXPECT_EQ(L"Waiting for the other screen to send it" + kEllipsis,
              text_of(window, MSWindowsPasteProgress::kSpeed));

    // how much has come, then how fast and how long is left
    progress.update(paste("00000000000000a1", Stage::SENDING, 100 * kMB, true));
    ASSERT_TRUE(eventually([&]() {
        return text_of(window, MSWindowsPasteProgress::kAmount) == L"100 MB of 400 MB (25%)";
    }));
    EXPECT_EQ(250, SendMessageW(GetDlgItem(window, MSWindowsPasteProgress::kBar), PBM_GETPOS,
                                0, 0));
    Sleep(1100);
    progress.update(paste("00000000000000a1", Stage::SENDING, 200 * kMB, true));
    ASSERT_TRUE(eventually([&]() {
        return text_of(window, MSWindowsPasteProgress::kSpeed).find(L"/s, about ") !=
               std::wstring::npos;
    }));

    // news of another paste ending changes nothing
    progress.update(paste("00000000000000a0", Stage::DONE));
    Sleep(100);
    EXPECT_TRUE(visible());

    // Cancel calls the paste off, once
    click(window, MSWindowsPasteProgress::kCancel);
    ASSERT_TRUE(eventually([&]() { return !cancels().empty(); }));
    click(window, MSWindowsPasteProgress::kCancel);
    progress.update(paste("00000000000000a1", Stage::SENDING, 250 * kMB, true));
    ASSERT_TRUE(eventually([&]() {
        return text_of(window, MSWindowsPasteProgress::kAmount) == L"250 MB of 400 MB (62%)";
    }));
    EXPECT_EQ(L"Cancelling" + kEllipsis, text_of(window, MSWindowsPasteProgress::kSpeed));
    EXPECT_FALSE(IsWindowEnabled(GetDlgItem(window, MSWindowsPasteProgress::kCancel)));
    EXPECT_EQ(std::vector<std::string>{"00000000000000a1"}, cancels());

    // and the window goes when the paste ends
    progress.update(paste("00000000000000a1", Stage::FAILED));
    ASSERT_TRUE(eventually([&]() { return !visible(); }));

    // the next paste starts afresh, and closing the window cancels it too
    progress.update(paste("00000000000000a2", Stage::WAITING));
    ASSERT_TRUE(eventually(visible));
    EXPECT_TRUE(IsWindowEnabled(GetDlgItem(window, MSWindowsPasteProgress::kCancel)));
    SendMessageW(window, WM_CLOSE, 0, 0);
    ASSERT_TRUE(eventually([&]() { return cancels().size() == 2; }));
    EXPECT_EQ("00000000000000a2", cancels()[1]);
    EXPECT_TRUE(visible());
}

TEST_F(MSWindowsPasteProgressWindowTests, senderThatDoesntCountShowsItIsWorking)
{
    MSWindowsPasteProgress progress([](const std::string&) {}, std::chrono::milliseconds(0));
    HWND window = progress.window();
    ASSERT_NE(nullptr, window);
    progress.update(paste("00000000000000a1", Stage::SENDING));
    ASSERT_TRUE(eventually([&]() { return progress.shown(); }));
    EXPECT_EQ(L"The other screen is sending it" + kEllipsis,
              text_of(window, MSWindowsPasteProgress::kSpeed));
    LONG_PTR style = GetWindowLongPtrW(GetDlgItem(window, MSWindowsPasteProgress::kBar),
                                       GWL_STYLE);
    EXPECT_NE(0, style & PBS_MARQUEE);
}

// shows the window on the desktop for a moment and saves a picture of it to
// the .bmp file LEAPDESK_PROGRESS_CAPTURE names, to see how it looks.  With
// LEAPDESK_PROGRESS_MANIFEST naming the server's manifest, its controls look
// as they do in the server and client.
TEST(MSWindowsPasteProgressTests, DISABLED_showsOnTheDesktop)
{
    wchar_t capture[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LEAPDESK_PROGRESS_CAPTURE", capture, MAX_PATH) == 0) {
        GTEST_SKIP() << "LEAPDESK_PROGRESS_CAPTURE isn't set";
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t manifest[MAX_PATH] = {};
    ULONG_PTR cookie = 0;
    HANDLE context = INVALID_HANDLE_VALUE;
    if (GetEnvironmentVariableW(L"LEAPDESK_PROGRESS_MANIFEST", manifest, MAX_PATH) != 0) {
        ACTCTXW actctx{};
        actctx.cbSize = sizeof(actctx);
        actctx.lpSource = manifest;
        context = CreateActCtxW(&actctx);
        ASSERT_NE(INVALID_HANDLE_VALUE, context);
        ActivateActCtx(context, &cookie);
    }

    {
        MSWindowsPasteProgress progress([](const std::string&) {}, std::chrono::milliseconds(0));
        Progress update = paste("00000000000000a1", Stage::SENDING, 100 * kMB, true);
        update.name = "Quarterly business review - final version with appendices (back).pptx";
        progress.update(update);
        Sleep(1100);
        update.sent = 131 * kMB;
        progress.update(update);
        Sleep(600);

        HWND window = progress.window();
        RECT frame{};
        GetWindowRect(window, &frame);
        int width = frame.right - frame.left;
        int height = frame.bottom - frame.top;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
        HGDIOBJ old = SelectObject(dc, bitmap);
        EXPECT_TRUE(PrintWindow(window, dc, PW_RENDERFULLCONTENT));
        SelectObject(dc, old);

        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = width;
        header.biHeight = height;
        header.biPlanes = 1;
        header.biBitCount = 24;
        header.biCompression = BI_RGB;
        std::vector<char> pixels(static_cast<std::size_t>((width * 3 + 3) & ~3) * height);
        GetDIBits(dc, bitmap, 0, height, pixels.data(), reinterpret_cast<BITMAPINFO*>(&header),
                  DIB_RGB_COLORS);
        BITMAPFILEHEADER file{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(header);
        file.bfSize = static_cast<DWORD>(file.bfOffBits + pixels.size());
        std::ofstream out(capture, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));

        DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
    }
    if (context != INVALID_HANDLE_VALUE) {
        DeactivateActCtx(0, cookie);
        ReleaseActCtx(context);
    }
}

} // namespace inputleap
