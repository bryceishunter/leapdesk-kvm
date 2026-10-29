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

#include "platform/MSWindowsPasteAgentHost.h"
#include "platform/MSWindowsPasteChannel.h"
#include "inputleap/Clipboard.h"
#include "inputleap/FileClip.h"

#include <gtest/gtest.h>

#include <shellapi.h>
#include <shlobj.h>

#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
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
        path_ = std::wstring(base) + L"leapdesk-paste-helper-" +
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

std::wstring widen(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

// the helper next to the tests, or empty
std::wstring helper_program()
{
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring helper(module);
    helper.resize(helper.find_last_of(L"\\/"));
    helper += L"\\leapdesk-server.exe";
    return GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES ? L"" : helper;
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

} // namespace

TEST(MSWindowsPasteChannelTests, messagesRoundTrip)
{
    PasteMessage message("offer", {"7", std::string("\0binary\xff", 8), ""});
    std::string framed = MSWindowsPasteChannel::encode(message);
    PasteMessage back;
    ASSERT_TRUE(MSWindowsPasteChannel::decode(framed.substr(4), back));
    EXPECT_EQ("offer", back.type);
    ASSERT_EQ(3u, back.fields.size());
    EXPECT_EQ(std::string("\0binary\xff", 8), back.fields[1]);
    EXPECT_EQ("", back.field(2));
    EXPECT_EQ("", back.field(9));

    // broken data is refused, not read past
    EXPECT_FALSE(MSWindowsPasteChannel::decode(framed.substr(4, framed.size() - 6), back));
    EXPECT_FALSE(MSWindowsPasteChannel::decode(std::string("\x05\0\0\0", 4), back));
    EXPECT_FALSE(MSWindowsPasteChannel::decode("", back));
}

class MSWindowsPasteAgentTests : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!in_private_window_station()) {
            GTEST_SKIP() << "puts data on the clipboard; run in a window station of its own";
        }
        program_ = helper_program();
        if (program_.empty()) {
            GTEST_SKIP() << "leapdesk-server.exe not next to the tests";
        }
        ASSERT_TRUE(SUCCEEDED(OleInitialize(nullptr)));
        initialized_ = true;
    }

    void TearDown() override
    {
        host_.reset();
        for (auto& thread : threads_) {
            thread.join();
        }
        if (initialized_) {
            OleSetClipboard(nullptr);
            OleUninitialize();
        }
    }

    // a host whose helper sends files like the screen that copied them would:
    // they show up in received_ under their transfer names
    void start(const FileClip& clip, const std::vector<std::string>& contents)
    {
        MSWindowsPasteAgentHost::Settings settings;
        settings.program = program_;
        settings.helper.staging_folder = staging_.path();
        settings.helper.received_folder = received_.path();
        settings.helper.own_address = "100.64.0.9";
        settings.helper.log_level = "DEBUG";
        // this test program isn't Explorer, but should paste like it
        settings.helper.refuse_all_paths = true;
        settings.request = [this, clip, contents](const FilePasteRequest& request) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(request);
            }
            threads_.emplace_back([this, clip, contents, request]() {
                host_->status({request.request, FilePasteState::SENDING, "sending"});
                for (std::size_t i = 0; i < clip.files.size(); ++i) {
                    received_.file(widen(clip.transfer_name(request.request, i)), contents[i]);
                }
                host_->status({request.request, FilePasteState::SENT, ""});
            });
        };
        host_ = std::make_unique<MSWindowsPasteAgentHost>(std::move(settings));
    }

    std::vector<FilePasteRequest> requests()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    std::wstring program_;
    Folder received_;
    Folder staging_;
    std::unique_ptr<MSWindowsPasteAgentHost> host_;
    bool initialized_ = false;

private:
    std::mutex mutex_;
    std::vector<FilePasteRequest> requests_;
    std::vector<std::thread> threads_;
};

TEST_F(MSWindowsPasteAgentTests, helperOffersFilesAndFetchesThemOnlyForAPaste)
{
    FileClip clip;
    clip.files.push_back({"report.txt", 6, 133000000000000000ull});
    clip.files.push_back({"data.bin", 3, 133000000000000000ull});
    clip.id = "fedcba9876543210";
    start(clip, {"report", "abc"});
    ASSERT_TRUE(host_->wait_connected(std::chrono::seconds(15)));
    host_->set_connected(true);

    Clipboard clipboard;
    clipboard.open(0);
    clipboard.clear();
    clipboard.add(IClipboard::kText, "with text");
    clipboard.add(IClipboard::kFiles, clip.marshall());
    clipboard.close();
    ASSERT_TRUE(host_->offer(clipboard));

    // the helper puts the files on the clipboard, as virtual files
    UINT descriptor = RegisterClipboardFormatW(L"FileGroupDescriptorW");
    bool offered = false;
    for (int i = 0; i < 500 && !offered; ++i) {
        offered = IsClipboardFormatAvailable(descriptor) != FALSE;
        if (!offered) {
            Sleep(10);
        }
    }
    ASSERT_TRUE(offered);
    EXPECT_TRUE(host_->owns_clipboard());
    Sleep(200);
    EXPECT_TRUE(requests().empty());

    // pasting has them sent, once, and they land under their own names
    Folder pasted;
    ASSERT_TRUE(paste_into(pasted.path()));
    EXPECT_EQ("report", read_file(pasted.path() + L"\\report.txt"));
    EXPECT_EQ("abc", read_file(pasted.path() + L"\\data.bin"));
    ASSERT_EQ(1u, requests().size());
    EXPECT_EQ(clip.id, requests()[0].file_id);
    EXPECT_EQ("100.64.0.9", requests()[0].address);

    // the same files offered again (the server's other clipboard) change nothing
    ASSERT_TRUE(host_->offer(clipboard));
    Folder again;
    ASSERT_TRUE(paste_into(again.path()));
    EXPECT_EQ("report", read_file(again.path() + L"\\report.txt"));
    EXPECT_EQ(1u, requests().size());

    // something else copied here is no longer the helper's
    ASSERT_TRUE(OpenClipboard(nullptr));
    EmptyClipboard();
    CloseClipboard();
    EXPECT_FALSE(host_->owns_clipboard());
}

} // namespace inputleap
