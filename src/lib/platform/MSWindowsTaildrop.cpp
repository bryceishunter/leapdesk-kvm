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

#include "platform/MSWindowsTaildrop.h"

#include "platform/MSWindowsSessionUser.h"
#include "common/win32/encoding_utilities.h"
#include "base/Log.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <userenv.h>

namespace inputleap {

namespace {

// the last line \p output printed, which is where tailscale says what went wrong
std::string last_line(const std::string& output)
{
    std::string::size_type end = output.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) {
        return {};
    }
    std::string::size_type begin = output.find_last_of("\r\n", end);
    begin = (begin == std::string::npos) ? 0 : begin + 1;
    return output.substr(begin, end + 1 - begin);
}

std::wstring widen(const std::string& text)
{
    return utf8_to_win_char(text).data();
}

} // namespace

MSWindowsTaildrop::MSWindowsTaildrop(Report report, Options options) :
    report_(std::move(report)),
    options_(std::move(options))
{
}

void MSWindowsTaildrop::cancel(const std::string& request)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!busy_ || sending_ != request || cancelled_) {
        return;
    }
    cancelled_ = true;
    if (process_ != nullptr) {
        TerminateProcess(process_, 1);
    }
    LOG_NOTE("stopped sending the files for paste %s: the paste was cancelled", request.c_str());
}

MSWindowsTaildrop::~MSWindowsTaildrop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        if (process_ != nullptr) {
            TerminateProcess(process_, 1);
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool MSWindowsTaildrop::send(const std::string& request, const std::vector<std::wstring>& paths,
                             const std::vector<std::string>& names, const std::string& address)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ || stopping_) {
        return false;
    }
    busy_ = true;
    sending_ = request;
    cancelled_ = false;

    // the thread of the previous paste has finished, or busy_ would be set
    if (thread_.joinable()) {
        thread_.join();
    }
    thread_ = std::thread([this, request, paths, names, address]() {
        run(request, paths, names, address);
    });
    return true;
}

void MSWindowsTaildrop::run(const std::string& request, const std::vector<std::wstring>& paths,
                            const std::vector<std::string>& names, const std::string& address)
{
    FilePasteStatus result{request, FilePasteState::SENT, ""};
    std::wstring program = options_.feed_command ? std::wstring() : find_program();
    if (!options_.feed_command && program.empty()) {
        result.state = FilePasteState::FAILED;
        result.detail = "Tailscale is not installed on the screen that copied the files";
    }
    else {
        report(FilePasteStatus{request, FilePasteState::SENDING,
                               "sending " + std::to_string(paths.size()) + " file(s) to " +
                               address + " with Taildrop"});

        // fed, the files are read here, so how much there is to send is known
        std::uint64_t total = 0;
        if (options_.feed) {
            ImpersonateSessionUser as_user;
            for (const auto& path : paths) {
                WIN32_FILE_ATTRIBUTE_DATA info;
                if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
                    total += (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
                             info.nFileSizeLow;
                }
            }
        }
        std::uint64_t sent = 0;

        for (std::size_t i = 0; i < paths.size(); ++i) {
            DWORD exit_code = 0;
            std::string output;
            if (options_.feed) {
                std::wstring command = options_.feed_command ?
                    options_.feed_command(names[i], address) :
                    quote_argument(program) + L" file cp --update-interval=0 " +
                        quote_argument(L"--name=" + widen(names[i])) + L" - " +
                        quote_argument(widen(address + ":"));
                std::string failure;
                if (!feed_program(program, command, paths[i], sent, total, request, exit_code,
                                  output, failure)) {
                    result.state = FilePasteState::FAILED;
                    result.detail = failure;
                    break;
                }
            }
            else if (!run_program(program, command_line(program, paths[i], names[i], address),
                                  exit_code, output)) {
                result.state = FilePasteState::FAILED;
                result.detail = "couldn't run tailscale.exe";
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancelled_) {
                    result.state = FilePasteState::FAILED;
                    result.detail = "the paste was cancelled";
                    break;
                }
            }
            if (exit_code != 0) {
                result.state = FilePasteState::FAILED;
                result.detail = "Taildrop failed";
                std::string reason = last_line(output);
                if (!reason.empty()) {
                    result.detail += ": " + reason;
                }
                break;
            }
        }
        if (options_.feed && result.state == FilePasteState::SENT) {
            report(FilePasteStatus{request, FilePasteState::SENDING,
                                   file_paste_progress(sent, total)});
        }
    }

    if (result.state == FilePasteState::SENT) {
        LOG_NOTE("sent %zu file(s) for paste %s", paths.size(), request.c_str());
    }
    else {
        LOG_NOTE("couldn't send the files for paste %s: %s", request.c_str(),
                 result.detail.c_str());
    }
    report(result);
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    sending_.clear();
}

bool MSWindowsTaildrop::feed_program(const std::wstring& program, const std::wstring& command,
                                     const std::wstring& path, std::uint64_t& sent,
                                     std::uint64_t total, const std::string& request,
                                     DWORD& exit_code, std::string& output, std::string& failure)
{
    // opened as the signed-in user, who sees the drives they mapped (a no-op
    // when this already runs as that user); the handle then reads as anyone
    HANDLE file = INVALID_HANDLE_VALUE;
    {
        ImpersonateSessionUser as_user;
        file = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) {
        failure = "couldn't read " + win_wchar_to_utf8(path.c_str()) + " (error " +
                  std::to_string(GetLastError()) + ")";
        return false;
    }

    Child child;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || cancelled_) {
            CloseHandle(file);
            failure = cancelled_ ? "the paste was cancelled" : "Leapdesk is stopping";
            return false;
        }
        if (!start_program(program, command, child, true)) {
            CloseHandle(file);
            failure = "couldn't run tailscale.exe";
            return false;
        }
        process_ = child.process;
    }

    // its output is read on a thread of its own, so that it can't fill up and
    // stop the program while this writes its input
    std::string collected;
    HANDLE output_pipe = child.output;
    std::thread reader([output_pipe, &collected]() {
        char buffer[512];
        DWORD count = 0;
        while (ReadFile(output_pipe, buffer, sizeof(buffer), &count, nullptr) && count > 0) {
            if (collected.size() < 8192) {
                collected.append(buffer, count);
            }
        }
    });

    // tailscale reads its input only as fast as it sends, so what has been
    // written is what has gone
    std::vector<char> buffer(256 * 1024);
    auto last_report = std::chrono::steady_clock::now();
    bool ok = true;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            failure = "couldn't read " + win_wchar_to_utf8(path.c_str()) + " (error " +
                      std::to_string(GetLastError()) + ")";
            ok = false;
            break;
        }
        if (count == 0) {
            break;
        }
        DWORD done = 0;
        while (ok && done < count) {
            DWORD written = 0;
            if (!WriteFile(child.input, buffer.data() + done, count - done, &written, nullptr) ||
                written == 0) {
                // it stopped reading: it failed, or it was stopped
                ok = false;
            }
            done += written;
        }
        if (!ok) {
            failure = "Taildrop stopped";
            break;
        }
        sent += count;
        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= options_.progress_interval) {
            last_report = now;
            report(FilePasteStatus{request, FilePasteState::SENDING,
                                   file_paste_progress(sent, total)});
        }
    }
    CloseHandle(file);

    // the end of its input ends the file; a file only part read mustn't
    // arrive looking whole, so the program is stopped first
    if (!ok) {
        TerminateProcess(child.process, 1);
    }
    CloseHandle(child.input);
    child.input = nullptr;
    WaitForSingleObject(child.process, INFINITE);
    reader.join();
    CloseHandle(child.output);
    child.output = nullptr;
    GetExitCodeProcess(child.process, &exit_code);
    output = collected;
    if (!ok && failure == "Taildrop stopped" && !last_line(collected).empty()) {
        failure = "Taildrop failed: " + last_line(collected);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = nullptr;
        if (cancelled_) {
            failure = "the paste was cancelled";
            ok = false;
        }
    }
    CloseHandle(child.process);
    return ok;
}

void MSWindowsTaildrop::report(const FilePasteStatus& status)
{
    // nobody is listening once the screen that owns us is going away
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_) {
        report_(status);
    }
}

bool MSWindowsTaildrop::run_program(const std::wstring& program, const std::wstring& command,
                                    DWORD& exit_code, std::string& output)
{
    Child child;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !start_program(program, command, child)) {
            return false;
        }
        process_ = child.process;
    }
    finish_program(child, exit_code, output);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = nullptr;
    }
    CloseHandle(child.process);
    return true;
}

bool MSWindowsTaildrop::start_program(const std::wstring& program, const std::wstring& command,
                                      Child& child, bool input)
{
    // the output goes to a pipe, so that an error message can be reported
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inheritable, 0)) {
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    // and when fed, the input comes from one.  a small buffer keeps what has
    // been written close to what has been sent
    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    if (input) {
        if (!CreatePipe(&input_read, &input_write, &inheritable, 64 * 1024)) {
            CloseHandle(read_end);
            CloseHandle(write_end);
            return false;
        }
        SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
    }

    // let it inherit only the pipes, not every inheritable handle of this
    // process, such as the sockets to the other screens
    HANDLE inherited[2] = {write_end, input_read};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> list_buffer(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_buffer.data());
    bool list_ready = InitializeProcThreadAttributeList(attributes, 1, 0, &size) != FALSE;
    bool ok = list_ready &&
              UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                        inherited, (input ? 2 : 1) * sizeof(HANDLE), nullptr,
                                        nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write_end;
    startup.StartupInfo.hStdError = write_end;
    startup.StartupInfo.hStdInput = input_read;
    startup.lpAttributeList = attributes;
    const wchar_t* application = program.empty() ? nullptr : program.c_str();

    // as the service runs us as SYSTEM, run tailscale as the user signed in to
    // this session: it's their files and their tailnet, and a drive they
    // mapped is only there for them
    PROCESS_INFORMATION process{};
    std::wstring writable_command = command;
    const DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    if (ok) {
        HANDLE token = session_user_token();
        if (token != nullptr) {
            // a SYSTEM token has these, but not necessarily switched on
            enable_privilege(SE_ASSIGNPRIMARYTOKEN_NAME);
            enable_privilege(SE_INCREASE_QUOTA_NAME);
            void* environment = nullptr;
            CreateEnvironmentBlock(&environment, token, FALSE);
            ok = CreateProcessAsUserW(token, application, &writable_command[0], nullptr,
                                      nullptr, TRUE, flags | CREATE_UNICODE_ENVIRONMENT,
                                      environment, nullptr, &startup.StartupInfo,
                                      &process) != FALSE;
            DWORD error = GetLastError();
            if (environment != nullptr) {
                DestroyEnvironmentBlock(environment);
            }
            CloseHandle(token);
            SetLastError(error);
        }
        else {
            if (running_as_system()) {
                LOG_NOTE("running tailscale.exe as SYSTEM, which doesn't see the user's "
                         "mapped drives");
            }
            ok = CreateProcessW(application, &writable_command[0], nullptr, nullptr, TRUE,
                                flags, nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
        }
    }
    DWORD error = GetLastError();
    if (list_ready) {
        DeleteProcThreadAttributeList(attributes);
    }
    // closing our copies lets reading end when the program exits, and lets the
    // program see the end of its input when ours closes
    CloseHandle(write_end);
    if (input_read != nullptr) {
        CloseHandle(input_read);
    }
    if (!ok) {
        LOG_WARN("couldn't run tailscale.exe: %d", error);
        CloseHandle(read_end);
        if (input_write != nullptr) {
            CloseHandle(input_write);
        }
        return false;
    }
    CloseHandle(process.hThread);
    child.process = process.hProcess;
    child.output = read_end;
    child.input = input_write;
    return true;
}

void MSWindowsTaildrop::finish_program(Child& child, DWORD& exit_code, std::string& output)
{
    char buffer[512];
    DWORD count = 0;
    while (ReadFile(child.output, buffer, sizeof(buffer), &count, nullptr) && count > 0) {
        if (output.size() < 8192) {
            output.append(buffer, count);
        }
    }
    CloseHandle(child.output);
    child.output = nullptr;

    WaitForSingleObject(child.process, INFINITE);
    GetExitCodeProcess(child.process, &exit_code);
}

std::string MSWindowsTaildrop::own_address()
{
    std::wstring program = find_program();
    Child child;
    if (program.empty() || !start_program(program, quote_argument(program) + L" ip -4", child)) {
        return {};
    }
    DWORD exit_code = 1;
    std::string output;
    finish_program(child, exit_code, output);
    CloseHandle(child.process);

    std::string address = output.substr(0, output.find_first_of("\r\n"));
    if (exit_code != 0 || !is_valid_paste_address(address)) {
        LOG_NOTE("couldn't get this machine's Tailscale address: %s", last_line(output).c_str());
        return {};
    }
    return address;
}

std::wstring MSWindowsTaildrop::received_folder()
{
    // Tailscale saves what it receives in the signed-in user's Downloads
    HANDLE token = session_user_token();
    PWSTR path = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, token, &path))) {
        folder = path;
    }
    CoTaskMemFree(path);
    if (token != nullptr) {
        CloseHandle(token);
    }
    return folder;
}

std::wstring MSWindowsTaildrop::command_line(const std::wstring& program, const std::wstring& path,
                                             const std::string& name, const std::string& address)
{
    // no progress lines, so that the last line is the error, if there is one
    return quote_argument(program) + L" file cp --update-interval=0 " +
           quote_argument(L"--name=" + widen(name)) + L" " + quote_argument(path) + L" " +
           quote_argument(widen(address + ":"));
}

std::wstring MSWindowsTaildrop::quote_argument(const std::wstring& argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }

    // the rules of CommandLineToArgvW: backslashes are literal unless they
    // precede a quote, where each pair makes one backslash
    std::wstring quoted = L"\"";
    for (auto c = argument.begin(); ; ++c) {
        std::size_t backslashes = 0;
        while (c != argument.end() && *c == L'\\') {
            ++c;
            ++backslashes;
        }
        if (c == argument.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*c == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        }
        else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(*c);
    }
    quoted.push_back(L'"');
    return quoted;
}

std::wstring MSWindowsTaildrop::find_program()
{
    // only where the Tailscale installer puts it.  this may run as SYSTEM, so
    // a tailscale.exe in a folder on the PATH that users can write to must
    // not be picked up instead
    wchar_t folder[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"ProgramFiles", folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    std::wstring program = std::wstring(folder) + L"\\Tailscale\\tailscale.exe";
    if (GetFileAttributesW(program.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return program;
}

} // namespace inputleap
