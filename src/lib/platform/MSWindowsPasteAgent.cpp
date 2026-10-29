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

#include "platform/MSWindowsPasteAgent.h"

#include "platform/MSWindowsPasteChannel.h"
#include "platform/MSWindowsPasteProgress.h"
#include "platform/MSWindowsVirtualFileOffer.h"
#include "platform/MSWindowsFileFetch.h"
#include "platform/MSWindowsFilePaste.h"
#include "platform/MSWindowsTaildrop.h"
#include "platform/MSWindowsClipboard.h"
#include "platform/MSWindowsClipboardUTF16Converter.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"
#include "platform/MSWindowsClipboardHTMLConverter.h"
#include "inputleap/Clipboard.h"
#include "inputleap/FileClip.h"
#include "arch/Arch.h"
#include "base/ILogOutputter.h"
#include "base/Log.h"
#include "common/win32/encoding_utilities.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <thread>

namespace inputleap {

namespace {

// posted to the helper's window, with a PasteMessage* to handle on its thread
const UINT kMessage = WM_APP + 1;
const wchar_t kWindowClass[] = L"LeapdeskPasteHelper";

std::string narrow(const std::wstring& text)
{
    return win_wchar_to_utf8(text.c_str());
}

// sends the helper's log to the service, which writes it into its own log
class ChannelLogOutputter : public ILogOutputter {
public:
    explicit ChannelLogOutputter(std::shared_ptr<MSWindowsPasteChannel> channel) :
        channel_(std::move(channel)) {}

    void open(const char*) override {}
    void close() override {}
    void show(bool) override {}

    bool write(ELevel level, const char* message) override
    {
        // drop the "[time] LEVEL: " the service will put in front again
        std::string text = message;
        if (!text.empty() && text[0] == '[') {
            std::size_t bracket = text.find("] ");
            std::size_t colon = bracket == std::string::npos ? bracket : text.find(": ", bracket);
            if (colon != std::string::npos) {
                text = text.substr(colon + 2);
            }
        }
        channel_->write(PasteMessage("log", {std::to_string(static_cast<int>(level)), text}));
        return true;
    }

private:
    std::shared_ptr<MSWindowsPasteChannel> channel_;
};

MSWindowsFileFetch::Settings fetch_settings(const MSWindowsPasteAgent::Options& options,
                                            std::shared_ptr<MSWindowsPasteChannel> channel)
{
    MSWindowsFileFetch::Settings settings;
    settings.request = [channel](const FilePasteRequest& request) {
        channel->write(PasteMessage("request",
                                    {request.request, request.file_id, request.address}));
    };
    if (options.own_address.empty()) {
        settings.own_address = &MSWindowsTaildrop::own_address;
    }
    else {
        std::string address = options.own_address;
        settings.own_address = [address]() { return address; };
    }
    if (options.received_folder.empty()) {
        settings.received_folder = &MSWindowsTaildrop::received_folder;
    }
    else {
        std::wstring folder = options.received_folder;
        settings.received_folder = [folder]() { return folder; };
    }
    settings.staging_folder = options.staging_folder;
    return settings;
}

class Agent {
public:
    Agent(const MSWindowsPasteAgent::Options& options,
          std::shared_ptr<MSWindowsPasteChannel> channel) :
        options_(options),
        channel_(std::move(channel)),
        fetch_(with_progress(fetch_settings(options_, channel_))),
        stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        // cancelling the paste also tells the screen sending the files to stop
        progress_ = std::make_unique<MSWindowsPasteProgress>([this](const std::string& request) {
            if (fetch_.cancel(request)) {
                LOG_NOTE("the paste was cancelled");
                channel_->write(PasteMessage("cancelled", {request}));
            }
        });
    }

    int run()
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &Agent::window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = kWindowClass;
        RegisterClassExW(&window_class);
        window_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        if (window_ == nullptr) {
            return 3;
        }
        SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        channel_->write(PasteMessage("hello", {std::to_string(GetCurrentProcessId())}));
        std::thread reader([this]() { read_loop(); });

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        // the service has gone: what's on offer can't be pasted any more.
        // emptying the clipboard, rather than leaving it to Windows, keeps it
        // from asking for every format (and so fetching the files) on the way
        SetEvent(stop_);
        fetch_.stop();
        if (current_ != nullptr && OleIsCurrentClipboard(current_) == S_OK) {
            OleSetClipboard(nullptr);
        }
        reader.join();
        DestroyWindow(window_);
        return 0;
    }

private:
    MSWindowsFileFetch::Settings with_progress(MSWindowsFileFetch::Settings settings)
    {
        settings.progress = [this](const MSWindowsFileFetch::Progress& progress) {
            if (progress_ != nullptr) {
                progress_->update(progress);
            }
        };
        return settings;
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == kMessage) {
            auto agent = reinterpret_cast<Agent*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            std::unique_ptr<PasteMessage> paste(reinterpret_cast<PasteMessage*>(lparam));
            if (agent != nullptr) {
                agent->handle(*paste);
            }
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void read_loop()
    {
        PasteMessage message;
        while (channel_->read(message, stop_)) {
            if (message.type == "status") {
                fetch_.status(FilePasteStatus{
                    message.field(0),
                    static_cast<FilePasteState>(std::atoi(message.field(1).c_str())),
                    message.field(2)});
            }
            else if (message.type == "connected") {
                fetch_.set_connected(message.field(0) == "1");
            }
            else {
                PostMessageW(window_, kMessage, 0,
                             reinterpret_cast<LPARAM>(new PasteMessage(message)));
            }
        }
        // the service has gone, or is stopping us
        fetch_.stop();
        PostMessageW(window_, kMessage, 0, reinterpret_cast<LPARAM>(new PasteMessage("gone")));
    }

    void handle(const PasteMessage& message)
    {
        if (message.type == "offer") {
            offer(std::strtoull(message.field(0).c_str(), nullptr, 10), message.field(1));
        }
        else if (message.type == "supersede") {
            superseded_ = (std::max)(superseded_,
                                     std::strtoull(message.field(0).c_str(), nullptr, 10));
        }
        else if (message.type == "gone") {
            PostQuitMessage(0);
        }
    }

    void offer(std::uint64_t generation, const std::string& data)
    {
        if (generation <= superseded_) {
            LOG_DEBUG("dropped an offer of files that something newer replaced");
            return;
        }

        Clipboard clipboard;
        clipboard.unmarshall(data, 0);
        FileClip clip;
        std::vector<std::pair<UINT, std::string>> formats;
        if (clipboard.open(0)) {
            std::string files = clipboard.has(IClipboard::kFiles) ?
                                clipboard.get(IClipboard::kFiles) : std::string();
            if (!FileClip::unmarshall(files, clip)) {
                clipboard.close();
                LOG_WARN("couldn't offer the files: the offer held none");
                return;
            }
            // whatever else was copied with them goes on the clipboard too
            MSWindowsClipboardUTF16Converter text;
            MSWindowsClipboardBitmapConverter bitmap;
            MSWindowsClipboardHTMLConverter html;
            for (IMSWindowsClipboardConverter* converter :
                 std::initializer_list<IMSWindowsClipboardConverter*>{&text, &bitmap, &html}) {
                if (!clipboard.has(converter->getFormat())) {
                    continue;
                }
                HANDLE handle = converter->fromIClipboard(clipboard.get(converter->getFormat()));
                if (handle != nullptr) {
                    const char* bytes = static_cast<const char*>(GlobalLock(handle));
                    if (bytes != nullptr) {
                        formats.emplace_back(converter->getWin32Format(),
                                             std::string(bytes, GlobalSize(handle)));
                        GlobalUnlock(handle);
                    }
                    GlobalFree(handle);
                }
            }
            clipboard.close();
        }

        MSWindowsVirtualFileOffer::Settings settings;
        if (options_.settle_ms >= 0) {
            settings.settle_time = std::chrono::milliseconds(options_.settle_ms);
        }
        if (options_.refuse_all_paths) {
            settings.refuse_paths = [](const std::string&) { return true; };
        }
        MSWindowsFileFetch* fetch = &fetch_;
        MSWindowsVirtualFileOffer* files = MSWindowsVirtualFileOffer::create(
            clip, std::move(formats),
            [fetch, clip](std::vector<std::wstring>& paths, std::string& failure) {
                return fetch->fetch(clip, "a paste", paths, failure);
            },
            settings);

        // files pasted from an earlier offer have been copied to wherever they
        // were pasted; those still being copied stay
        MSWindowsFilePaste::clean_staging(options_.staging_folder, L"");

        HRESULT result = OleSetClipboard(files);
        if (FAILED(result)) {
            LOG_WARN("couldn't offer files copied on another screen: 0x%08lx",
                     static_cast<unsigned long>(result));
            files->Release();
            return;
        }
        if (current_ != nullptr) {
            current_->Release();
        }
        current_ = files;
        channel_->write(PasteMessage("placed", {std::to_string(generation),
                                                std::to_string(GetClipboardSequenceNumber())}));
        LOG_NOTE("%zu file(s) copied on another screen can be pasted here", clip.files.size());
    }

    MSWindowsPasteAgent::Options options_;
    std::shared_ptr<MSWindowsPasteChannel> channel_;
    MSWindowsFileFetch fetch_;
    std::unique_ptr<MSWindowsPasteProgress> progress_;
    HANDLE stop_;
    HWND window_ = nullptr;
    MSWindowsVirtualFileOffer* current_ = nullptr;
    std::uint64_t superseded_ = 0;
};

HANDLE connect_to(const std::wstring& pipe)
{
    for (int attempt = 0; attempt < 150; ++attempt) {
        HANDLE handle = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            return handle;
        }
        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipe.c_str(), 100);
        }
        else {
            Sleep(100);
        }
    }
    return nullptr;
}

} // namespace

std::wstring MSWindowsPasteAgent::arguments(const Options& options)
{
    std::wstring args = L"--leapdesk-paste-agent --pipe " +
                        MSWindowsTaildrop::quote_argument(options.pipe) + L" --staging " +
                        MSWindowsTaildrop::quote_argument(options.staging_folder) + L" --level " +
                        utf8_to_win_char(options.log_level).data();
    if (!options.received_folder.empty()) {
        args += L" --received " + MSWindowsTaildrop::quote_argument(options.received_folder);
    }
    if (!options.own_address.empty()) {
        args += std::wstring(L" --address ") + utf8_to_win_char(options.own_address).data();
    }
    if (options.settle_ms >= 0) {
        args += L" --settle " + std::to_wstring(options.settle_ms);
    }
    if (options.refuse_all_paths) {
        args += L" --refuse-all-paths";
    }
    return args;
}

bool MSWindowsPasteAgent::run_if_asked(int, char**, int& exit_code)
{
    // the wide command line, so folders with any name survive
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args == nullptr) {
        return false;
    }
    bool asked = false;
    Options options;
    for (int i = 1; i < count; ++i) {
        std::wstring arg = args[i];
        auto next = [&]() { return i + 1 < count ? std::wstring(args[++i]) : std::wstring(); };
        if (arg == L"--leapdesk-paste-agent") {
            asked = true;
        }
        else if (!asked) {
            continue;
        }
        else if (arg == L"--pipe") {
            options.pipe = next();
        }
        else if (arg == L"--staging") {
            options.staging_folder = next();
        }
        else if (arg == L"--level") {
            options.log_level = narrow(next());
        }
        else if (arg == L"--received") {
            options.received_folder = next();
        }
        else if (arg == L"--address") {
            options.own_address = narrow(next());
        }
        else if (arg == L"--settle") {
            options.settle_ms = _wtoi(next().c_str());
        }
        else if (arg == L"--refuse-all-paths") {
            options.refuse_all_paths = true;
        }
    }
    LocalFree(args);
    if (!asked) {
        return false;
    }
    exit_code = run(options);
    return true;
}

int MSWindowsPasteAgent::run(const Options& options)
{
    Arch arch;
    arch.init();
    Log log;
    log.setFilter(options.log_level.c_str());

    if (options.pipe.empty() || options.staging_folder.empty()) {
        return 1;
    }
    HRESULT ole = OleInitialize(nullptr);
    HANDLE pipe = connect_to(options.pipe);
    if (pipe == nullptr) {
        if (SUCCEEDED(ole)) {
            OleUninitialize();
        }
        return 2;
    }
    auto channel = std::make_shared<MSWindowsPasteChannel>(pipe);
    log.insert(new ChannelLogOutputter(channel));

    // left to the end of the process on purpose: a paste in progress may
    // still be using it on a thread of its own
    auto agent = new Agent(options, channel);
    int code = agent->run();
    if (SUCCEEDED(ole)) {
        OleUninitialize();
    }
    return code;
}

} // namespace inputleap
