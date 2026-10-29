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

#include "platform/MSWindowsSessionUser.h"

#include "base/Log.h"

#include <userenv.h>
#include <wtsapi32.h>

#include <vector>

namespace inputleap {

bool running_as_system()
{
    static const bool system = []() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return false;
        }
        char buffer[256];
        DWORD size = 0;
        bool result = GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size) &&
            IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, WinLocalSystemSid);
        CloseHandle(token);
        return result;
    }();
    return system;
}

bool enable_privilege(const char* name)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool result = LookupPrivilegeValueA(nullptr, name, &privileges.Privileges[0].Luid) &&
                  AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) &&
                  GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return result;
}

HANDLE session_user_token()
{
    if (!running_as_system()) {
        return nullptr;
    }

    // a SYSTEM token has this privilege, but not necessarily switched on
    enable_privilege(SE_TCB_NAME);
    DWORD session = 0;
    HANDLE token = nullptr;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) ||
        !WTSQueryUserToken(session, &token)) {
        LOG_WARN("couldn't get the token of the user signed in to session %u: %d", session,
                 GetLastError());
        return nullptr;
    }
    return token;
}

std::string session_user_name()
{
    HANDLE token = session_user_token();
    if (token == nullptr) {
        return {};
    }
    char buffer[256];
    DWORD size = 0;
    std::string result;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
        char name[256];
        char domain[256];
        DWORD name_size = sizeof(name);
        DWORD domain_size = sizeof(domain);
        SID_NAME_USE use;
        if (LookupAccountSidA(nullptr, reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, name,
                              &name_size, domain, &domain_size, &use)) {
            result = std::string(domain) + "\\" + name;
        }
    }
    CloseHandle(token);
    return result;
}

bool run_as_session_user(const std::wstring& command, DWORD& exit_code, std::string& output,
                         DWORD timeout_ms)
{
    // capture the program's stdout through a pipe
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inheritable, 0)) {
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    // inherit only the pipe, not this process's other handles (its sockets)
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> list_buffer(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_buffer.data());
    bool ok = InitializeProcThreadAttributeList(attributes, 1, 0, &size) != FALSE &&
              UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                        &write_end, sizeof(write_end), nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write_end;
    startup.StartupInfo.hStdError = write_end;
    // the interactive desktop, so the program can reach the user's clipboard
    wchar_t desktop[] = L"winsta0\\default";
    startup.StartupInfo.lpDesktop = desktop;
    startup.lpAttributeList = attributes;

    PROCESS_INFORMATION process{};
    std::wstring writable_command = command;
    const DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    HANDLE token = session_user_token();
    if (ok && token != nullptr) {
        enable_privilege(SE_ASSIGNPRIMARYTOKEN_NAME);
        enable_privilege(SE_INCREASE_QUOTA_NAME);
        void* environment = nullptr;
        CreateEnvironmentBlock(&environment, token, FALSE);
        ok = CreateProcessAsUserW(token, nullptr, &writable_command[0], nullptr, nullptr, TRUE,
                                  flags, environment, nullptr, &startup.StartupInfo,
                                  &process) != FALSE;
        if (environment != nullptr) {
            DestroyEnvironmentBlock(environment);
        }
    }
    else if (ok) {
        // already the user
        ok = CreateProcessW(nullptr, &writable_command[0], nullptr, nullptr, TRUE,
                            flags & ~static_cast<DWORD>(EXTENDED_STARTUPINFO_PRESENT), nullptr,
                            nullptr, &startup.StartupInfo, &process) != FALSE;
    }
    DWORD error = GetLastError();
    if (token != nullptr) {
        CloseHandle(token);
    }
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(write_end);
    if (!ok) {
        LOG_WARN("couldn't run '%ls' as the signed-in user: %d", command.c_str(), error);
        CloseHandle(read_end);
        return false;
    }
    CloseHandle(process.hThread);
    LOG_DEBUG("started '%ls' as the signed-in user, pid %lu", command.c_str(),
              static_cast<unsigned long>(process.dwProcessId));

    // collect the output until the program exits, but never wait on it for
    // longer than the timeout: a stuck program mustn't stall the caller
    ULONGLONG started = GetTickCount64();
    bool exited = false;
    char buffer[1024];
    for (;;) {
        DWORD available = 0;
        while (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) &&
               available > 0) {
            DWORD count = 0;
            DWORD want = available < sizeof(buffer) ? available : sizeof(buffer);
            if (!ReadFile(read_end, buffer, want, &count, nullptr) || count == 0) {
                break;
            }
            if (output.size() < (1u << 20)) {
                output.append(buffer, count);
            }
        }
        if (exited) {
            break;  // drained what it wrote before exiting
        }
        if (WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0) {
            exited = true;
            continue;
        }
        if (GetTickCount64() - started > timeout_ms) {
            break;
        }
    }
    CloseHandle(read_end);

    if (exited) {
        GetExitCodeProcess(process.hProcess, &exit_code);
        LOG_DEBUG("'%ls' finished in %llu ms with exit code 0x%08lx", command.c_str(),
                  GetTickCount64() - started, static_cast<unsigned long>(exit_code));
    }
    else {
        LOG_WARN("'%ls', run as the signed-in user, didn't finish within %lu ms",
                 command.c_str(), static_cast<unsigned long>(timeout_ms));
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hProcess);
    return exited;
}

ImpersonateSessionUser::ImpersonateSessionUser()
{
    if (!running_as_system()) {
        return;
    }
    HANDLE token = session_user_token();
    if (token != nullptr) {
        impersonating_ = ImpersonateLoggedOnUser(token) != FALSE;
        if (!impersonating_) {
            LOG_WARN("couldn't act as the signed-in user: %d", GetLastError());
        }
        CloseHandle(token);
    }
    failed_ = !impersonating_;
}

ImpersonateSessionUser::~ImpersonateSessionUser()
{
    if (impersonating_) {
        RevertToSelf();
    }
}

} // namespace inputleap
