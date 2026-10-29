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

#include "platform/MSWindowsFilePaste.h"

#include "platform/MSWindowsClipboard.h"
#include "platform/MSWindowsSessionUser.h"
#include "inputleap/Clipboard.h"
#include "common/win32/encoding_utilities.h"
#include "base/Log.h"

#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <random>

namespace inputleap {

namespace {

const UINT kOfferMessage = WM_APP + 1;
const UINT kStopMessage = WM_APP + 2;
const wchar_t kWindowClass[] = L"LeapdeskFilePaste";

std::wstring widen(const std::string& text)
{
    return utf8_to_win_char(text).data();
}

std::string narrow(const std::wstring& text)
{
    return win_wchar_to_utf8(text.c_str());
}

bool is_link(DWORD attributes)
{
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// deletes \p folder and what is in it, as far as it can.  a link is deleted
// itself, never followed
void remove_tree(const std::wstring& folder)
{
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = entry.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            std::wstring path = folder + L"\\" + name;
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                DeleteFileW(path.c_str());
            }
            else if (is_link(entry.dwFileAttributes)) {
                RemoveDirectoryW(path.c_str());
            }
            else {
                remove_tree(path);
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    RemoveDirectoryW(folder.c_str());
}

// puts \p value on the open clipboard in the registered format \p name
void set_clipboard_dword(const wchar_t* name, DWORD value)
{
    UINT format = RegisterClipboardFormatW(name);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (format == 0 || data == nullptr) {
        if (data != nullptr) {
            GlobalFree(data);
        }
        return;
    }
    *static_cast<DWORD*>(GlobalLock(data)) = value;
    GlobalUnlock(data);
    if (SetClipboardData(format, data) == nullptr) {
        GlobalFree(data);
    }
}

// the program asking for the clipboard's data, which has the clipboard open
// meanwhile, usually with a window of its own
std::string clipboard_requester()
{
    HWND window = GetOpenClipboardWindow();
    if (window == nullptr) {
        return "a program that opened the clipboard without a window";
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    std::string name = "process " + std::to_string(pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            std::wstring image(path, size);
            std::wstring::size_type slash = image.find_last_of(L"\\/");
            name = narrow(slash == std::wstring::npos ? image : image.substr(slash + 1)) +
                   " (" + std::to_string(pid) + ")";
        }
        CloseHandle(process);
    }
    return name;
}

// Windows clipboard history, the cloud clipboard and clipboard managers can
// read each new clipboard item in full to keep a copy.  for files offered
// here, reading them is a paste, which has them sent over.  these formats ask
// them to leave the item alone.  (Explorer pays them no attention; see
// Settings::settle_time.)  the clipboard must be open, by its owner
void keep_out_of_clipboard_history()
{
    set_clipboard_dword(L"ExcludeClipboardContentFromMonitorProcessing", 0);
    set_clipboard_dword(L"CanIncludeInClipboardHistory", 0);
    set_clipboard_dword(L"CanUploadToCloudClipboard", 0);
}

} // namespace

struct MSWindowsFilePaste::Offer {
    std::uint64_t generation = 0;
    Clipboard clipboard;
    FileClip clip;
};

namespace {

MSWindowsFileFetch::Settings fetch_settings(const MSWindowsFilePaste::Settings& settings)
{
    MSWindowsFileFetch::Settings fetch;
    fetch.request = settings.request;
    fetch.own_address = settings.own_address;
    fetch.received_folder = settings.received_folder;
    fetch.staging_folder = settings.staging_folder;
    fetch.answer_timeout = settings.answer_timeout;
    fetch.arrival_timeout = settings.arrival_timeout;
    fetch.transfer_timeout = settings.transfer_timeout;
    return fetch;
}

} // namespace

MSWindowsFilePaste::MSWindowsFilePaste(Settings settings) :
    settings_(std::move(settings)),
    fetch_(fetch_settings(settings_))
{
    thread_ = std::thread([this]() { run(); });
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this]() { return ready_; });
}

MSWindowsFilePaste::~MSWindowsFilePaste()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        pending_.reset();
    }
    changed_.notify_all();
    fetch_.stop();
    if (window_ != nullptr) {
        PostMessageW(window_, kStopMessage, 0, 0);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool MSWindowsFilePaste::offer(const IClipboard& clipboard)
{
    if (window_ == nullptr) {
        return false;
    }
    auto offer = std::make_unique<Offer>();
    IClipboard::copy(&offer->clipboard, &clipboard);
    std::string files;
    if (offer->clipboard.open(0)) {
        if (offer->clipboard.has(IClipboard::kFiles)) {
            files = offer->clipboard.get(IClipboard::kFiles);
        }
        offer->clipboard.close();
    }
    if (!FileClip::unmarshall(files, offer->clip)) {
        return false;
    }

    offer->generation = ++generation_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(offer);
    }
    PostMessageW(window_, kOfferMessage, 0, 0);
    return true;
}

void MSWindowsFilePaste::supersede()
{
    ++generation_;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
}

void MSWindowsFilePaste::status(const FilePasteStatus& status)
{
    fetch_.status(status);
}

void MSWindowsFilePaste::set_connected(bool connected)
{
    fetch_.set_connected(connected);
}

void MSWindowsFilePaste::run()
{
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &MSWindowsFilePaste::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClass;
    // registered once per process; the error for a second time doesn't matter
    RegisterClassExW(&window_class);

    // a message-only window: it only has to own the clipboard
    HWND window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  window_class.hInstance, this);
    if (window == nullptr) {
        LOG_ERR("couldn't create the window for pasting files: %d", GetLastError());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        window_ = window;
        ready_ = true;
    }
    changed_.notify_all();
    if (window == nullptr) {
        return;
    }

    // files left over from pastes before a restart
    {
        ImpersonateSessionUser as_user;
        if (!as_user.failed()) {
            clean_staging(settings_.staging_folder, L"");
        }
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        DispatchMessageW(&message);
    }
}

LRESULT CALLBACK MSWindowsFilePaste::window_proc(HWND window, UINT message, WPARAM wparam,
                                                 LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto self = reinterpret_cast<MSWindowsFilePaste*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case kOfferMessage:
        self->place();
        return 0;

    case WM_RENDERFORMAT:
        // an application is pasting and waits until this returns
        if (wparam == CF_HDROP) {
            self->render();
        }
        return 0;

    case WM_RENDERALLFORMATS:
        // the window is going away.  nobody pasted, so nothing is fetched
        return 0;

    case WM_DESTROYCLIPBOARD:
        self->lost_clipboard();
        return 0;

    case kStopMessage:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void MSWindowsFilePaste::place()
{
    std::unique_ptr<Offer> offer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        offer = std::move(pending_);
    }
    if (offer == nullptr) {
        return;
    }

    MSWindowsClipboard clipboard(window_);
    if (!clipboard.open(0)) {
        LOG_WARN("couldn't offer files copied on another screen: the clipboard is in use");
        return;
    }

    // something newer may have been put on the clipboard meanwhile.  and the
    // same files come twice when the server sets both of its clipboards,
    // which mustn't undo a paste that has fetched them already
    bool latest = offer->generation == generation_;
    bool already_offered = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        already_offered = offered_ != nullptr && offered_->id == offer->clip.id;
    }
    // clear() takes ownership; our previous offer, if any, is told it's gone
    if (latest && !(already_offered && GetClipboardOwner() == window_) && clipboard.clear()) {
        if (offer->clipboard.open(0)) {
            for (int format = 0; format != IClipboard::kNumFormats; ++format) {
                auto id = static_cast<IClipboard::EFormat>(format);
                if (id != IClipboard::kFiles && offer->clipboard.has(id)) {
                    clipboard.add(id, offer->clipboard.get(id));
                }
            }
            offer->clipboard.close();
        }
        // no data: Windows asks for it when an application pastes
        SetClipboardData(CF_HDROP, nullptr);
        keep_out_of_clipboard_history();

        std::lock_guard<std::mutex> lock(mutex_);
        offered_ = std::make_unique<FileClip>(offer->clip);
        offered_at_ = std::chrono::steady_clock::now();
        LOG_NOTE("%zu file(s) copied on another screen can be pasted here",
                 offer->clip.files.size());
    }
    clipboard.close();
}

void MSWindowsFilePaste::render()
{
    FileClip clip;
    std::chrono::steady_clock::time_point offered_at;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (offered_ == nullptr) {
            return;
        }
        clip = *offered_;
        offered_at = offered_at_;
    }

    // Explorer reads the file list the moment files land on the clipboard,
    // to show cut files dimmed, and asks a few times more within moments if
    // it gets nothing.  answering that would fetch the files over the network
    // though nobody pasted.  a paste comes later, and Windows asks again then
    std::string requester = clipboard_requester();
    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - offered_at);
    if (since < settings_.settle_time) {
        LOG_DEBUG("not handing over the files to %s %lld ms after offering them",
                  requester.c_str(), static_cast<long long>(since.count()));
        return;
    }

    std::vector<std::wstring> paths;
    std::string failure;
    if (!fetch_.fetch(clip, requester, paths, failure)) {
        return;
    }

    // the application has the clipboard open, so it isn't opened here
    HGLOBAL drop = make_drop(paths);
    if (drop != nullptr && SetClipboardData(CF_HDROP, drop) == nullptr) {
        LOG_WARN("couldn't hand over the pasted files: %d", GetLastError());
        GlobalFree(drop);
    }
}

void MSWindowsFilePaste::lost_clipboard()
{
    // what was pasted from the staging folder has been copied to wherever it
    // was pasted, and can't be pasted again
    {
        std::lock_guard<std::mutex> lock(mutex_);
        offered_.reset();
    }
    ImpersonateSessionUser as_user;
    if (!as_user.failed()) {
        clean_staging(settings_.staging_folder, L"");
    }
}

HGLOBAL MSWindowsFilePaste::make_drop(const std::vector<std::wstring>& paths)
{
    // a DROPFILES header, then the paths, each ending in a null, then a null
    std::wstring list;
    for (const auto& path : paths) {
        list += path;
        list += L'\0';
    }
    list += L'\0';

    SIZE_T size = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (data == nullptr) {
        return nullptr;
    }
    auto drop = static_cast<DROPFILES*>(GlobalLock(data));
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES), list.data(),
                list.size() * sizeof(wchar_t));
    GlobalUnlock(data);
    return data;
}

std::wstring MSWindowsFilePaste::find_received(const std::wstring& folder,
                                               const std::string& prefix,
                                               const std::string& name)
{
    std::wstring exact = folder + L"\\" + widen(name);
    DWORD attributes = GetFileAttributesW(exact.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return exact;
    }

    // Tailscale may have changed the name; the prefix is ours alone.  a file
    // still being written would end in .partial
    std::wstring found;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW((folder + L"\\" + widen(prefix) + L"*").c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring candidate = entry.cFileName;
            const std::wstring partial = L".partial";
            bool is_partial = candidate.size() > partial.size() &&
                candidate.compare(candidate.size() - partial.size(), partial.size(), partial) == 0;
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && !is_partial) {
                found = folder + L"\\" + candidate;
                break;
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    return found;
}

void MSWindowsFilePaste::clean_staging(const std::wstring& staging_folder,
                                       const std::wstring& keep)
{
    // only folders this made are deleted: named like a paste request, and not
    // reached through a link
    DWORD attributes = GetFileAttributesW(staging_folder.c_str());
    if (staging_folder.empty() || attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || is_link(attributes)) {
        return;
    }

    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW((staging_folder + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        std::wstring name = entry.cFileName;
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            !is_link(entry.dwFileAttributes) && name != keep &&
            is_valid_paste_request(narrow(name))) {
            remove_tree(staging_folder + L"\\" + name);
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

std::string MSWindowsFilePaste::new_request()
{
    std::random_device random;
    std::uint64_t value = (static_cast<std::uint64_t>(random()) << 32) | random();
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

} // namespace inputleap
