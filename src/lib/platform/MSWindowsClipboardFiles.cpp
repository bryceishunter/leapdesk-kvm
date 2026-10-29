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

#include "platform/MSWindowsClipboardFiles.h"

#include "platform/MSWindowsSessionUser.h"
#include "inputleap/FileClip.h"
#include "common/win32/encoding_utilities.h"
#include "base/Log.h"

#include <ole2.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <sstream>

namespace inputleap {

void MSWindowsClipboardFiles::report_copy(const FileClip* clip, const std::string& problem)
{
    // the same copy can be noticed more than once; log it once
    static DWORD last_sequence = 0;
    DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == last_sequence) {
        return;
    }
    last_sequence = sequence;

    if (clip != nullptr) {
        std::string names;
        for (std::size_t i = 0; i < clip->files.size() && i < 3; ++i) {
            names += (i == 0 ? "" : ", ") + clip->files[i].name;
        }
        if (clip->files.size() > 3) {
            names += ", ...";
        }
        LOG_NOTE("copied %zu file(s) (%s), which can be pasted on the other screens",
                 clip->files.size(), names.c_str());
    }
    else {
        LOG_NOTE("copied files can't be pasted on the other screens: %s", problem.c_str());
    }
}

namespace {

std::string hr_text(HRESULT hr)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(hr));
    return text;
}

} // namespace

bool MSWindowsClipboardFiles::clipboard_file_paths(std::vector<std::wstring>& paths,
                                                            std::string* why)
{
    paths.clear();
    auto note = [why](const std::string& text) {
        if (why != nullptr) {
            *why = text;
        }
    };

    // files Leapdesk itself offers from another screen are only a promise:
    // reading them would fetch them
    if (IsClipboardFormatAvailable(RegisterClipboardFormatW(L"LeapdeskOwnership"))) {
        note("the clipboard holds files Leapdesk offers from another screen");
        return false;
    }

    IDataObject* object = nullptr;
    HRESULT hr = OleGetClipboard(&object);
    if (hr != S_OK || object == nullptr) {
        note("OleGetClipboard failed: " + hr_text(hr));
        return false;
    }

    FORMATETC format = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    hr = object->QueryGetData(&format);
    bool has_files = hr == S_OK;
    if (!has_files) {
        note("the clipboard holds no files (" + hr_text(hr) + ")");
    }
    else {
        STGMEDIUM medium = {};
        hr = object->GetData(&format, &medium);
        if (hr == S_OK) {
            if (medium.hGlobal != nullptr) {
                paths = get_paths(medium.hGlobal);
            }
            ReleaseStgMedium(&medium);
            if (paths.empty()) {
                note("the clipboard's file list is empty");
            }
        }
        else {
            note("the application that copied the files didn't hand over their names (" +
                 hr_text(hr) + ")");
        }
    }
    object->Release();
    return has_files;
}

namespace {

// the helper's answer starts with one of these lines
const char kOkHeader[] = "OK";      // the files, then their paths
const char kBadHeader[] = "BAD";    // files that can't be sent, then why
const char kNoneHeader[] = "NONE";  // no files, then why

// \p text made safe for a log line
std::string printable(const std::string& text, std::size_t limit = 200)
{
    std::string result;
    for (char c : text.substr(0, limit)) {
        result += (c == '\n') ? '|' : (static_cast<unsigned char>(c) < 0x20 ? '.' : c);
    }
    if (text.size() > limit) {
        result += "...";
    }
    return result;
}

std::string to_hex(const std::string& bytes)
{
    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        hex += digits[c >> 4];
        hex += digits[c & 0xf];
    }
    return hex;
}

bool from_hex(const std::string& hex, std::string& bytes)
{
    if (hex.size() % 2 != 0) {
        return false;
    }
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    bytes.clear();
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = value(hex[i]);
        int lo = value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bytes += static_cast<char>((hi << 4) | lo);
    }
    return true;
}

} // namespace

bool MSWindowsClipboardFiles::clipboard_maybe_has_files()
{
    // an application's file copy shows up either as CF_HDROP directly, or -
    // to a service that can't reach the data object - as an opaque OLE
    // "DataObject" wrapper.  either is a reason to look closer; plain text
    // is neither.
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        return true;
    }

    // a service running as SYSTEM sees the wrapper when it lists the formats,
    // yet IsClipboardFormatAvailable() of the wrapper's registered format came
    // back false there.  so list them, and match the wrapper by name.  the
    // application that just copied (or clipboard history) may still have the
    // clipboard open for a moment.
    bool opened = false;
    for (int attempt = 0; attempt < 20 && !opened; ++attempt) {
        opened = OpenClipboard(nullptr) != FALSE;
        if (!opened) {
            Sleep(10);
        }
    }
    if (!opened) {
        // can't tell; better to look closer than to miss a copy
        LOG_DEBUG("the clipboard is busy, so it may hold files: %lu", GetLastError());
        return true;
    }
    bool found = false;
    UINT format = 0;
    while (!found && (format = EnumClipboardFormats(format)) != 0) {
        wchar_t name[32];
        found = format == CF_HDROP ||
                (format >= 0xC000 && GetClipboardFormatNameW(format, name, 32) > 0 &&
                 lstrcmpW(name, L"DataObject") == 0);
    }
    CloseClipboard();
    LOG_DEBUG("the clipboard %s files", found ? "may hold" : "holds no");
    return found;
}

MSWindowsClipboardFiles::Files
MSWindowsClipboardFiles::read_clipboard_files()
{
    // the same copy is read when it happens and again whenever the pointer
    // leaves this screen.  reading it can mean starting a helper, so reuse
    // the answer until the clipboard changes
    static DWORD cached_sequence = 0;
    static Files cached;
    DWORD sequence = GetClipboardSequenceNumber();
    if (sequence != 0 && sequence == cached_sequence) {
        return cached;
    }
    Files result = read_clipboard_files_now();
    // a helper that failed to run says nothing about the clipboard: try again
    // next time rather than remember that
    if (result.present || result.answered) {
        cached_sequence = sequence;
        cached = result;
    }
    return result;
}

MSWindowsClipboardFiles::Files
MSWindowsClipboardFiles::read_clipboard_files_now()
{
    Files result;

    if (running_as_system()) {
        // a service can't read an application's OLE clipboard, so run a copy
        // of ourselves as the signed-in user to read it
        wchar_t self[MAX_PATH];
        if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
            LOG_NOTE("couldn't read the copied files: this program's path is unknown (%lu)",
                     GetLastError());
            return result;
        }
        std::wstring command = std::wstring(L"\"") + self + L"\" " +
                               utf8_to_win_char(kHelperOption).data();
        if (CLOG->getFilter() >= kDEBUG) {
            command += std::wstring(L" ") + utf8_to_win_char(kHelperLogOption).data();
        }
        DWORD exit_code = 1;
        std::string output;
        LOG_DEBUG("reading the copied files as the signed-in user");
        // it normally takes well under a second; don't hold up the screen for long
        if (!run_as_session_user(command, exit_code, output, 5000)) {
            LOG_NOTE("couldn't read the copied files: the helper didn't run as the signed-in "
                     "user");
            return result;
        }
        if (exit_code != 0) {
            LOG_NOTE("couldn't read the copied files: the helper run as the signed-in user "
                     "exited with 0x%08lx%s%s", static_cast<unsigned long>(exit_code),
                     output.empty() ? "" : ", saying: ", printable(output).c_str());
            return result;
        }

        std::size_t nl = output.find('\n');
        std::string header = output.substr(0, nl);
        std::string rest = nl == std::string::npos ? std::string() : output.substr(nl + 1);
        if (header == kNoneHeader) {
            rest.erase(rest.find_last_not_of("\r\n") + 1);
            LOG_DEBUG("no files on the signed-in user's clipboard: %s", printable(rest).c_str());
            result.answered = true;
            return result;
        }
        if (header == kBadHeader) {
            result.answered = true;
            result.present = true;
            result.problem = rest;
            result.problem.erase(result.problem.find_last_not_of("\r\n") + 1);
            return result;
        }
        if (header != kOkHeader) {
            LOG_NOTE("couldn't read the copied files: the helper answered \"%s\"",
                     printable(output).c_str());
            return result;
        }
        std::istringstream lines(rest);
        std::string clip_hex;
        std::getline(lines, clip_hex);
        clip_hex.erase(clip_hex.find_last_not_of("\r\n") + 1);
        std::string clip_bytes;
        if (!from_hex(clip_hex, clip_bytes) || !FileClip::unmarshall(clip_bytes, result.clip)) {
            LOG_NOTE("couldn't read the copied files: the helper's description of them "
                     "couldn't be decoded");
            return result;
        }
        std::string path;
        while (std::getline(lines, path)) {
            path.erase(path.find_last_not_of("\r\n") + 1);
            if (!path.empty()) {
                auto wide = utf8_to_win_char(path);
                result.paths.emplace_back(wide.data());
            }
        }
        result.answered = true;
        result.present = true;
        result.ok = result.paths.size() == result.clip.files.size();
        if (!result.ok) {
            result.problem = "the helper returned an inconsistent file list";
        }
        return result;
    }

    // running as the user: read the clipboard directly
    result.answered = true;
    result.present = clipboard_file_paths(result.paths);
    if (result.present) {
        result.ok = make_file_clip(result.paths, result.clip, &result.problem);
    }
    return result;
}

namespace {

// who and where the helper runs, for its log
std::string describe_helper()
{
    std::string text;
    wchar_t user[256];
    DWORD size = 256;
    text += "user=" + (GetUserNameW(user, &size) ? win_wchar_to_utf8(user) : std::string("?"));
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    text += " session=" + std::to_string(session);
    wchar_t name[128];
    DWORD needed = 0;
    if (GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, name, sizeof(name),
                                  &needed)) {
        text += " station=" + win_wchar_to_utf8(name);
    }
    if (GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name,
                                  sizeof(name), &needed)) {
        text += " desktop=" + win_wchar_to_utf8(name);
    }
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        char buffer[64];
        DWORD length = 0;
        if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &length)) {
            PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
            DWORD level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
            char text_level[16];
            std::snprintf(text_level, sizeof(text_level), "0x%04lx",
                          static_cast<unsigned long>(level));
            text += std::string(" integrity=") + text_level;
        }
        CloseHandle(token);
    }
    return text;
}

// appends \p line to clipboard-helper.log in the user's Leapdesk folder, so
// what the helper saw can be read even when its parent can't hear it
void log_helper(const std::string& line)
{
    wchar_t folder[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return;
    }
    std::wstring path = std::wstring(folder) + L"\\Leapdesk";
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\clipboard-helper.log";

    // start over rather than grow without bound
    WIN32_FILE_ATTRIBUTE_DATA info;
    bool too_big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) &&
                   (info.nFileSizeHigh != 0 || info.nFileSizeLow > 256 * 1024);
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | (too_big ? GENERIC_WRITE : 0),
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              too_big ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "[%04u-%02u-%02uT%02u:%02u:%02u] ", now.wYear,
                  now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::string text = stamp + line + "\r\n";
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

} // namespace

bool MSWindowsClipboardFiles::run_helper_if_asked(int argc, char** argv, int& exit_code)
{
    bool helper = false;
    bool keep_log = false;
    for (int i = 1; i < argc; ++i) {
        helper = helper || std::strcmp(argv[i], kHelperOption) == 0;
        keep_log = keep_log || std::strcmp(argv[i], kHelperLogOption) == 0;
    }
    if (helper) {
        exit_code = dump_to_stdout(keep_log);
    }
    return helper;
}

int MSWindowsClipboardFiles::dump_to_stdout(bool keep_log)
{
    // runs as the user; read the clipboard and print its files for the parent
    HRESULT init = OleInitialize(nullptr);
    std::vector<std::wstring> paths;
    std::string why;
    bool present = SUCCEEDED(init) && clipboard_file_paths(paths, &why);
    if (FAILED(init)) {
        why = "OleInitialize failed: " + hr_text(init);
    }

    std::string out;
    std::string summary;
    if (present) {
        FileClip clip;
        std::string problem;
        if (make_file_clip(paths, clip, &problem)) {
            out = std::string(kOkHeader) + "\n" + to_hex(clip.marshall()) + "\n";
            for (const auto& path : paths) {
                out += win_wchar_to_utf8(path.c_str()) + "\n";
            }
            summary = std::string(kOkHeader) + " " + std::to_string(clip.files.size()) +
                      " file(s)";
        }
        else {
            out = std::string(kBadHeader) + "\n" + problem + "\n";
            summary = std::string(kBadHeader) + " " + problem;
        }
    }
    else {
        out = std::string(kNoneHeader) + "\n" + why + "\n";
        summary = std::string(kNoneHeader) + " " + why;
    }

    // write straight to the handle: no newline translation, exact bytes
    HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    bool sent = stdout_handle != nullptr && stdout_handle != INVALID_HANDLE_VALUE &&
                WriteFile(stdout_handle, out.data(), static_cast<DWORD>(out.size()), &written,
                          nullptr);
    if (keep_log) {
        log_helper(describe_helper() + " -> " + summary +
                   (sent ? "" : " (couldn't answer: " + std::to_string(GetLastError()) + ")"));
    }

    if (SUCCEEDED(init)) {
        OleUninitialize();
    }
    return 0;
}

std::vector<std::wstring> MSWindowsClipboardFiles::get_paths(HANDLE drop)
{
    std::vector<std::wstring> paths;
    HDROP files = static_cast<HDROP>(drop);
    UINT count = DragQueryFileW(files, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(files, i, nullptr, 0);
        if (length == 0) {
            return {};
        }
        std::wstring path(length, L'\0');
        if (DragQueryFileW(files, i, &path[0], length + 1) != length) {
            return {};
        }
        paths.push_back(path);
    }
    return paths;
}

bool MSWindowsClipboardFiles::make_file_clip(const std::vector<std::wstring>& paths,
                                                      FileClip& clip, std::string* problem)
{
    auto fail = [problem](const std::string& reason) {
        if (problem != nullptr) {
            *problem = reason;
        }
        return false;
    };
    if (paths.empty()) {
        return fail("no files");
    }
    if (paths.size() > FileClip::kMaxFiles) {
        return fail("more than " + std::to_string(FileClip::kMaxFiles) + " files");
    }

    // look at the files as the signed-in user.  under the service this runs
    // as SYSTEM, which doesn't see the drives the user mapped, such as a share
    // on a NAS.  if that fails, SYSTEM may still see them
    ImpersonateSessionUser as_user;

    clip.files.clear();
    std::vector<std::string> utf8_paths;
    for (const auto& path : paths) {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
            DWORD error = GetLastError();
            return fail(win_wchar_to_utf8(path.c_str()) + " can't be read (error " +
                        std::to_string(error) + ")");
        }
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return fail(win_wchar_to_utf8(path.c_str()) + " is a folder");
        }

        std::wstring::size_type slash = path.find_last_of(L"\\/");
        std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);

        FileClip::File file;
        file.name = win_wchar_to_utf8(name.c_str());
        file.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        file.modified = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                        info.ftLastWriteTime.dwLowDateTime;
        clip.files.push_back(file);
        utf8_paths.push_back(win_wchar_to_utf8(path.c_str()));
    }
    clip.id = FileClip::make_id(utf8_paths, clip.files);
    return true;
}

} // namespace inputleap
