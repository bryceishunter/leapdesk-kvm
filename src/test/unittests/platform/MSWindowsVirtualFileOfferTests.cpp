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

#include "platform/MSWindowsVirtualFileOffer.h"
#include "inputleap/FileClip.h"

#include <gtest/gtest.h>

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace inputleap {

namespace {

bool in_private_window_station()
{
    wchar_t name[256] = {};
    DWORD needed = 0;
    GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, name, sizeof(name), &needed);
    return _wcsicmp(name, L"WinSta0") != 0;
}

class Folder {
public:
    Folder()
    {
        static int count = 0;
        wchar_t base[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        path_ = std::wstring(base) + L"leapdesk-virtual-files-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++count);
        CreateDirectoryW(path_.c_str(), nullptr);
    }
    ~Folder()
    {
        std::wstring from = path_ + L'\0';
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.c_str();
        operation.fFlags = FOF_NO_UI;
        SHFileOperationW(&operation);
    }
    std::wstring file(const std::wstring& name, const std::string& contents) const
    {
        std::wstring path = path_ + L"\\" + name;
        std::ofstream(path, std::ios::binary) << contents;
        return path;
    }
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

std::string read_file(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

FileClip clip_of(const std::vector<std::pair<std::string, std::string>>& files)
{
    FileClip clip;
    for (const auto& file : files) {
        clip.files.push_back({file.first, file.second.size(), 133000000000000000ull});
    }
    clip.id = "0123456789abcdef";
    return clip;
}

// drops the clipboard's data on \p folder, which is what Explorer's Paste does
bool paste_into(const std::wstring& folder)
{
    IDataObject* data = nullptr;
    if (FAILED(OleGetClipboard(&data))) {
        return false;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    IDropTarget* target = nullptr;
    HRESULT result = SHParseDisplayName(folder.c_str(), nullptr, &pidl, 0, nullptr);
    if (SUCCEEDED(result)) {
        result = SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parent), &child);
    }
    if (SUCCEEDED(result)) {
        result = parent->GetUIObjectOf(nullptr, 1, &child, IID_IDropTarget, nullptr,
                                       reinterpret_cast<void**>(&target));
    }
    if (SUCCEEDED(result)) {
        POINTL point{0, 0};
        DWORD effect = DROPEFFECT_COPY;
        result = target->DragEnter(data, 0, point, &effect);
        effect = DROPEFFECT_COPY;
        if (SUCCEEDED(result)) {
            result = target->Drop(data, 0, point, &effect);
        }
    }
    if (target != nullptr) {
        target->Release();
    }
    if (parent != nullptr) {
        parent->Release();
    }
    CoTaskMemFree(pidl);
    data->Release();
    return SUCCEEDED(result);
}

// the paths a program gets asking for CF_HDROP, with the clipboard opened by
// \p window (or by no window)
std::vector<std::wstring> paths_for(HWND window)
{
    std::vector<std::wstring> paths;
    if (!OpenClipboard(window)) {
        return paths;
    }
    HANDLE drop = GetClipboardData(CF_HDROP);
    if (drop != nullptr) {
        UINT count = DragQueryFileW(static_cast<HDROP>(drop), 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH];
            DragQueryFileW(static_cast<HDROP>(drop), i, path, MAX_PATH);
            paths.push_back(path);
        }
    }
    CloseClipboard();
    return paths;
}

} // namespace

TEST(MSWindowsVirtualFileOfferTests, descriptorDescribesTheFiles)
{
    FileClip clip = clip_of({{"report.pdf", "12345"}, {"a/b:c.txt", ""}});
    std::string data = MSWindowsVirtualFileOffer::make_descriptor(clip);
    ASSERT_EQ(sizeof(FILEGROUPDESCRIPTORW) + sizeof(FILEDESCRIPTORW), data.size());
    auto group = reinterpret_cast<const FILEGROUPDESCRIPTORW*>(data.data());
    ASSERT_EQ(2u, group->cItems);
    EXPECT_STREQ(L"report.pdf", group->fgd[0].cFileName);
    EXPECT_EQ(5u, group->fgd[0].nFileSizeLow);
    EXPECT_EQ(0u, group->fgd[0].nFileSizeHigh);
    EXPECT_NE(0u, group->fgd[0].dwFlags & FD_FILESIZE);
    EXPECT_NE(0u, group->fgd[0].dwFlags & FD_PROGRESSUI);
    std::uint64_t written = (static_cast<std::uint64_t>(group->fgd[0].ftLastWriteTime.dwHighDateTime) << 32) |
                            group->fgd[0].ftLastWriteTime.dwLowDateTime;
    EXPECT_EQ(133000000000000000ull, written);
    // a name can't reach outside the folder it's pasted into
    EXPECT_STREQ(L"a_b_c.txt", group->fgd[1].cFileName);
}

TEST(MSWindowsVirtualFileOfferTests, pathsAreRefusedToExplorerAndUnnamedPrograms)
{
    EXPECT_TRUE(MSWindowsVirtualFileOffer::refuses_paths_to("explorer.exe"));
    EXPECT_TRUE(MSWindowsVirtualFileOffer::refuses_paths_to(""));
    EXPECT_FALSE(MSWindowsVirtualFileOffer::refuses_paths_to("notepad.exe"));
}

class MSWindowsVirtualFileOfferClipboardTests : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!in_private_window_station()) {
            GTEST_SKIP() << "puts data on the clipboard; run in a window station of its own";
        }
        ASSERT_TRUE(SUCCEEDED(OleInitialize(nullptr)));
        initialized_ = true;
        a_ = source_.file(L"a.txt", "alpha");
        b_ = source_.file(L"b.bin", std::string("\0\1\2\3", 4));
    }

    void TearDown() override
    {
        if (initialized_) {
            OleSetClipboard(nullptr);
            OleUninitialize();
        }
    }

    // an offer of a.txt and b.bin whose fetch can be told to fail.  pasting in
    // this test program, refused the paths like Explorer, takes the virtual files
    MSWindowsVirtualFileOffer* offer(bool like_explorer,
                                     std::chrono::milliseconds settle_time = {},
                                     std::chrono::milliseconds failure_hold = {})
    {
        MSWindowsVirtualFileOffer::Settings settings;
        settings.settle_time = settle_time;
        settings.failure_hold = failure_hold;
        if (like_explorer) {
            settings.refuse_paths = [](const std::string&) { return true; };
        }
        std::string text("h\0e\0l\0l\0o\0\0\0", 12);
        return MSWindowsVirtualFileOffer::create(
            clip_of({{"a.txt", "alpha"}, {"b.bin", std::string("\0\1\2\3", 4)}}),
            {{CF_UNICODETEXT, text}},
            [this](std::vector<std::wstring>& paths, std::string& failure) {
                Sleep(100);
                if (fail_next_) {
                    fail_next_ = false;
                    failure = "the screen that copied the files didn't answer";
                    return false;
                }
                paths = {a_, b_};
                return true;
            },
            settings);
    }

    Folder source_;
    std::wstring a_;
    std::wstring b_;
    std::atomic<bool> fail_next_{false};
    bool initialized_ = false;
};

TEST_F(MSWindowsVirtualFileOfferClipboardTests, filesAreFetchedOnlyWhenPasted)
{
    MSWindowsVirtualFileOffer* files = offer(true);
    ASSERT_EQ(S_OK, OleSetClipboard(files));

    // looking at what's on the clipboard fetches nothing
    IDataObject* data = nullptr;
    ASSERT_EQ(S_OK, OleGetClipboard(&data));
    FORMATETC descriptor{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW")),
                         nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    ASSERT_EQ(S_OK, data->GetData(&descriptor, &medium));
    auto group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
    EXPECT_EQ(2u, group->cItems);
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    FORMATETC text{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    ASSERT_EQ(S_OK, data->GetData(&text, &medium));
    EXPECT_STREQ(L"hello", static_cast<const wchar_t*>(GlobalLock(medium.hGlobal)));
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    data->Release();
    EXPECT_EQ(0, files->fetches());

    // pasting fetches them, once
    Folder pasted;
    ASSERT_TRUE(paste_into(pasted.path()));
    EXPECT_EQ("alpha", read_file(pasted.path() + L"\\a.txt"));
    EXPECT_EQ(std::string("\0\1\2\3", 4), read_file(pasted.path() + L"\\b.bin"));
    EXPECT_EQ(1, files->fetches());

    // and pasting again uses what was fetched
    Folder again;
    ASSERT_TRUE(paste_into(again.path()));
    EXPECT_EQ("alpha", read_file(again.path() + L"\\a.txt"));
    EXPECT_EQ(1, files->fetches());

    OleSetClipboard(nullptr);
    files->Release();
}

TEST_F(MSWindowsVirtualFileOfferClipboardTests, failedFetchFailsThatPasteOnly)
{
    MSWindowsVirtualFileOffer* files = offer(true, {}, std::chrono::milliseconds(300));
    ASSERT_EQ(S_OK, OleSetClipboard(files));

    // a failure fails the whole paste, without fetching again for the rest of it
    fail_next_ = true;
    Folder first;
    paste_into(first.path());
    EXPECT_FALSE(exists(first.path() + L"\\a.txt"));
    EXPECT_FALSE(exists(first.path() + L"\\b.bin"));
    EXPECT_EQ(1, files->fetches());
    Folder at_once;
    paste_into(at_once.path());
    EXPECT_FALSE(exists(at_once.path() + L"\\a.txt"));
    EXPECT_EQ(1, files->fetches());

    // a paste a moment later tries again
    Sleep(400);
    Folder second;
    ASSERT_TRUE(paste_into(second.path()));
    EXPECT_EQ("alpha", read_file(second.path() + L"\\a.txt"));
    EXPECT_EQ(2, files->fetches());

    OleSetClipboard(nullptr);
    files->Release();
}

TEST_F(MSWindowsVirtualFileOfferClipboardTests, pathsGoOnlyToNamedProgramsAfterAMoment)
{
    MSWindowsVirtualFileOffer* files = offer(false, std::chrono::milliseconds(300));
    ASSERT_EQ(S_OK, OleSetClipboard(files));
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  nullptr, nullptr);
    ASSERT_NE(nullptr, window);

    // clipboard watchers look at once
    EXPECT_TRUE(paths_for(window).empty());
    Sleep(400);
    // a program that doesn't say who it is might be Explorer
    EXPECT_TRUE(paths_for(nullptr).empty());
    EXPECT_EQ(0, files->fetches());

    // this one says, and gets the files
    EXPECT_EQ((std::vector<std::wstring>{a_, b_}), paths_for(window));
    EXPECT_EQ(1, files->fetches());

    DestroyWindow(window);
    OleSetClipboard(nullptr);
    files->Release();
}

} // namespace inputleap
