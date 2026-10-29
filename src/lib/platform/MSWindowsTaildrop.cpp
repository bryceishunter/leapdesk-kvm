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

MSWindowsTaildrop::MSWindowsTaildrop(Report report) :
    report_(std::move(report))
{
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
    std::wstring program = find_program();
    if (program.empty()) {
        result.state = FilePasteState::FAILED;
        result.detail = "Tailscale is not installed on the screen that copied the files";
    }
    else {
        report(FilePasteStatus{request, FilePasteState::SENDING,
                               "sending " + std::to_string(paths.size()) + " file(s) to " +
                               address + " with Taildrop"});
        for (std::size_t i = 0; i < paths.size(); ++i) {
            DWORD exit_code = 0;
            std::string output;
            if (!run_program(program, command_line(program, paths[i], names[i], address),
                             exit_code, output)) {
                result.state = FilePasteState::FAILED;
                result.detail = "couldn't run tailscale.exe";
                break;
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
                                      Child& child)
{
    // the output goes to a pipe, so that an error message can be reported
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inheritable, 0)) {
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    // let it inherit only the pipe, not every inheritable handle of this
    // process, such as the sockets to the other screens
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> list_buffer(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_buffer.data());
    bool list_ready = InitializeProcThreadAttributeList(attributes, 1, 0, &size) != FALSE;
    bool ok = list_ready &&
              UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                        &write_end, sizeof(write_end), nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write_end;
    startup.StartupInfo.hStdError = write_end;
    startup.lpAttributeList = attributes;

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
            ok = CreateProcessAsUserW(token, program.c_str(), &writable_command[0], nullptr,
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
            ok = CreateProcessW(program.c_str(), &writable_command[0], nullptr, nullptr, TRUE,
                                flags, nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
        }
    }
    DWORD error = GetLastError();
    if (list_ready) {
        DeleteProcThreadAttributeList(attributes);
    }
    // closing our copy lets reading end when the program exits
    CloseHandle(write_end);
    if (!ok) {
        LOG_WARN("couldn't run tailscale.exe: %d", error);
        CloseHandle(read_end);
        return false;
    }
    CloseHandle(process.hThread);
    child.process = process.hProcess;
    child.output = read_end;
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
