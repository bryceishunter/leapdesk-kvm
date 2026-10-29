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
#include "platform/MSWindowsSessionUser.h"
#include "platform/MSWindowsTaildrop.h"
#include "inputleap/Clipboard.h"
#include "base/Log.h"
#include "common/win32/encoding_utilities.h"

#include <sddl.h>
#include <userenv.h>

#include <algorithm>
#include <cstdlib>

namespace inputleap {

namespace {

// how long an offer counts as the helper's while it's on its way
const std::chrono::seconds kOfferInFlight(2);

// the user who may open the pipe: the one signed in, when this runs as SYSTEM
std::wstring pipe_user_sid()
{
    HANDLE token = session_user_token();
    if (token == nullptr && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    char buffer[256];
    DWORD size = 0;
    std::wstring result;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid)) {
            result = sid;
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return result;
}

} // namespace

MSWindowsPasteAgentHost::MSWindowsPasteAgentHost(Settings settings) :
    settings_(std::move(settings)),
    stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    thread_ = std::thread([this]() { run(); });
}

MSWindowsPasteAgentHost::~MSWindowsPasteAgentHost()
{
    SetEvent(stop_);
    if (thread_.joinable()) {
        thread_.join();
    }
    CloseHandle(stop_);
}

bool MSWindowsPasteAgentHost::connected() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return channel_ != nullptr && helper_ready_;
}

bool MSWindowsPasteAgentHost::wait_connected(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this]() { return channel_ != nullptr && helper_ready_; });
}

void MSWindowsPasteAgentHost::run()
{
    auto delay = settings_.restart_delay;
    while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
        std::wstring name = L"\\\\.\\pipe\\leapdesk-paste-" +
                            std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64());
        auto started = std::chrono::steady_clock::now();
        serve(name);
        if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) {
            break;
        }
        // start it again, waiting longer each time it doesn't last
        bool lasted = std::chrono::steady_clock::now() - started > std::chrono::minutes(1);
        if (lasted) {
            delay = settings_.restart_delay;
        }
        WaitForSingleObject(stop_, static_cast<DWORD>(delay.count()));
        if (!lasted) {
            delay = (std::min)(delay * 2, std::chrono::milliseconds(std::chrono::minutes(1)));
        }
    }
}

bool MSWindowsPasteAgentHost::serve(const std::wstring& pipe_name)
{
    HANDLE pipe = create_pipe(pipe_name);
    if (pipe == nullptr) {
        return false;
    }
    PROCESS_INFORMATION process{};
    if (!start_helper(pipe_name, process)) {
        CloseHandle(pipe);
        return false;
    }

    // wait for it to connect
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = ConnectNamedPipe(pipe, &overlapped) != FALSE;
    DWORD error = GetLastError();
    if (!connected && error == ERROR_PIPE_CONNECTED) {
        connected = true;
    }
    else if (!connected && error == ERROR_IO_PENDING) {
        HANDLE events[3] = {overlapped.hEvent, stop_, process.hProcess};
        DWORD count = 0;
        if (WaitForMultipleObjects(3, events, FALSE, 15000) == WAIT_OBJECT_0) {
            connected = GetOverlappedResult(pipe, &overlapped, &count, FALSE) != FALSE;
        }
        else {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &count, TRUE);
        }
    }
    CloseHandle(overlapped.hEvent);

    // only the helper this started may talk to it
    ULONG client = 0;
    if (connected && (!GetNamedPipeClientProcessId(pipe, &client) ||
                      client != process.dwProcessId)) {
        LOG_WARN("refused a connection to the paste helper's pipe from process %lu", client);
        connected = false;
    }

    if (connected) {
        auto channel = std::make_shared<MSWindowsPasteChannel>(pipe);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            channel_ = channel;
        }
        PasteMessage message;
        while (channel->read(message, stop_)) {
            handle(message);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            channel_.reset();
            helper_ready_ = false;
            in_flight_ = 0;
            placed_sequence_ = 0;
        }
        changed_.notify_all();
        // the pipe closes with the last reference, and the helper then exits
    }
    else {
        CloseHandle(pipe);
        if (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
            LOG_WARN("the paste helper didn't connect");
        }
    }

    if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    if (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
        LOG_NOTE("the paste helper exited (%lu); starting it again shortly",
                 static_cast<unsigned long>(exit_code));
    }
    CloseHandle(process.hProcess);
    return connected;
}

HANDLE MSWindowsPasteAgentHost::create_pipe(const std::wstring& name)
{
    std::wstring user = pipe_user_sid();
    if (user.empty()) {
        return nullptr;
    }
    // SYSTEM and the signed-in user, nobody else; never from another machine
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GRGW;;;" + user + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
        LOG_WARN("couldn't make the paste helper's pipe: %lu", GetLastError());
        return nullptr;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    HANDLE pipe = CreateNamedPipeW(name.c_str(),
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                       FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                       PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 64 * 1024, 64 * 1024, 0, &attributes);
    DWORD error = GetLastError();
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) {
        LOG_WARN("couldn't make the paste helper's pipe: %lu", error);
        return nullptr;
    }
    return pipe;
}

bool MSWindowsPasteAgentHost::start_helper(const std::wstring& pipe_name,
                                           PROCESS_INFORMATION& process)
{
    MSWindowsPasteAgent::Options options = settings_.helper;
    options.pipe = pipe_name;
    std::wstring command = MSWindowsTaildrop::quote_argument(settings_.program) + L" " +
                           MSWindowsPasteAgent::arguments(options);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    bool ok = false;
    DWORD error = 0;
    HANDLE token = session_user_token();
    if (token != nullptr) {
        // as the signed-in user, on their desktop, where their applications are
        wchar_t desktop[] = L"winsta0\\default";
        startup.lpDesktop = desktop;
        enable_privilege(SE_ASSIGNPRIMARYTOKEN_NAME);
        enable_privilege(SE_INCREASE_QUOTA_NAME);
        void* environment = nullptr;
        CreateEnvironmentBlock(&environment, token, FALSE);
        ok = CreateProcessAsUserW(token, nullptr, &command[0], nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment,
                                  nullptr, &startup, &process) != FALSE;
        error = GetLastError();
        if (environment != nullptr) {
            DestroyEnvironmentBlock(environment);
        }
        CloseHandle(token);
    }
    else if (running_as_system()) {
        // nobody is signed in yet; tried again later
        return false;
    }
    else {
        // already the user: the helper shares this process's desktop
        ok = CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &process) != FALSE;
        error = GetLastError();
    }
    if (!ok) {
        LOG_WARN("couldn't start the paste helper: %lu", static_cast<unsigned long>(error));
        return false;
    }
    CloseHandle(process.hThread);
    process.hThread = nullptr;
    LOG_DEBUG("started the paste helper, process %lu",
              static_cast<unsigned long>(process.dwProcessId));
    return true;
}

void MSWindowsPasteAgentHost::handle(const PasteMessage& message)
{
    if (message.type == "hello") {
        LOG_DEBUG("the paste helper is running, as process %s", message.field(0).c_str());
        {
            std::lock_guard<std::mutex> order(connected_mutex_);
            bool others = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                others = others_connected_;
            }
            send(PasteMessage("connected", {others ? "1" : "0"}));
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            helper_ready_ = true;
        }
        changed_.notify_all();
    }
    else if (message.type == "placed") {
        std::uint64_t generation = std::strtoull(message.field(0).c_str(), nullptr, 10);
        DWORD sequence = static_cast<DWORD>(std::strtoul(message.field(1).c_str(), nullptr, 10));
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == in_flight_) {
            in_flight_ = 0;
        }
        if (generation == generation_) {
            placed_sequence_ = sequence;
        }
    }
    else if (message.type == "request") {
        if (settings_.request) {
            settings_.request(FilePasteRequest{message.field(0), message.field(1),
                                               message.field(2)});
        }
    }
    else if (message.type == "cancelled") {
        if (settings_.cancelled && is_valid_paste_request(message.field(0))) {
            settings_.cancelled(message.field(0));
        }
    }
    else if (message.type == "log") {
        int level = std::atoi(message.field(0).c_str());
        level = (std::max)(static_cast<int>(kFATAL), (std::min)(level, static_cast<int>(kDEBUG5)));
        CLOG->print(static_cast<ELevel>(level), nullptr, 0, "paste helper: %s",
                    message.field(1).c_str());
    }
}

bool MSWindowsPasteAgentHost::send(const PasteMessage& message)
{
    std::shared_ptr<MSWindowsPasteChannel> channel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channel = channel_;
    }
    return channel != nullptr && channel->write(message);
}

bool MSWindowsPasteAgentHost::offer(const IClipboard& clipboard)
{
    Clipboard copy;
    IClipboard::copy(&copy, &clipboard);
    std::string files;
    if (copy.open(0)) {
        if (copy.has(IClipboard::kFiles)) {
            files = copy.get(IClipboard::kFiles);
        }
        copy.close();
    }
    FileClip clip;
    if (!FileClip::unmarshall(files, clip)) {
        return false;
    }

    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (channel_ == nullptr || !helper_ready_) {
            return false;
        }
        // the same files come twice when the server sets both of its
        // clipboards; offering them again would undo a paste in progress
        if (clip.id == placed_files_ && owns_clipboard_locked()) {
            return true;
        }
        generation = ++generation_;
        in_flight_ = generation;
        sent_at_ = std::chrono::steady_clock::now();
        placed_sequence_ = 0;
        placed_files_ = clip.id;
    }
    if (!send(PasteMessage("offer", {std::to_string(generation), IClipboard::marshall(&copy)}))) {
        std::lock_guard<std::mutex> lock(mutex_);
        in_flight_ = 0;
        return false;
    }
    return true;
}

void MSWindowsPasteAgentHost::supersede()
{
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation = ++generation_;
        in_flight_ = 0;
        placed_files_.clear();
    }
    send(PasteMessage("supersede", {std::to_string(generation)}));
}

void MSWindowsPasteAgentHost::status(const FilePasteStatus& status)
{
    send(PasteMessage("status", {status.request, std::to_string(static_cast<int>(status.state)),
                                 status.detail}));
}

void MSWindowsPasteAgentHost::set_connected(bool connected)
{
    std::lock_guard<std::mutex> order(connected_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        others_connected_ = connected;
    }
    send(PasteMessage("connected", {connected ? "1" : "0"}));
}

bool MSWindowsPasteAgentHost::owns_clipboard() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return owns_clipboard_locked();
}

bool MSWindowsPasteAgentHost::owns_clipboard_locked() const
{
    if (in_flight_ != 0 && std::chrono::steady_clock::now() - sent_at_ < kOfferInFlight) {
        return true;
    }
    return placed_sequence_ != 0 && GetClipboardSequenceNumber() == placed_sequence_;
}

} // namespace inputleap
