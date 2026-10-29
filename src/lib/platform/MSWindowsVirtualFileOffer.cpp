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

#include "platform/MSWindowsFileFetch.h"
#include "platform/MSWindowsFilePaste.h"
#include "common/win32/encoding_utilities.h"
#include "base/Log.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <thread>

namespace inputleap {

namespace {

// what a paste that couldn't get the files answers: one the user called off
// is cancelled, which applications take as no error to report
HRESULT paste_failed(const std::string& failure)
{
    if (failure == MSWindowsFileFetch::kCancelled) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    LOG_NOTE("couldn't paste the files: %s", failure.c_str());
    return E_FAIL;
}

// a new global memory block holding \p bytes
HGLOBAL make_global(const std::string& bytes)
{
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes.empty() ? 1 : bytes.size());
    if (data == nullptr) {
        return nullptr;
    }
    void* target = GlobalLock(data);
    if (!bytes.empty()) {
        std::memcpy(target, bytes.data(), bytes.size());
    }
    GlobalUnlock(data);
    return data;
}

std::string dword_bytes(DWORD value)
{
    return std::string(reinterpret_cast<const char*>(&value), sizeof(value));
}

FORMATETC format_of(UINT format, DWORD tymed)
{
    return FORMATETC{static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, tymed};
}

// a file name the shell will take as one: no folders, no drive
std::wstring safe_file_name(const std::string& name)
{
    auto wide = utf8_to_win_char(name);
    std::wstring result(wide.data());
    for (auto& c : result) {
        if (c == L'\\' || c == L'/' || c == L':' || c < 0x20) {
            c = L'_';
        }
    }
    return result.empty() ? L"file" : result;
}

} // namespace

MSWindowsVirtualFileOffer* MSWindowsVirtualFileOffer::create(
    const FileClip& clip, std::vector<std::pair<UINT, std::string>> other_formats, Fetch fetch,
    Settings settings)
{
    return new MSWindowsVirtualFileOffer(clip, std::move(other_formats), std::move(fetch),
                                         std::move(settings));
}

MSWindowsVirtualFileOffer::MSWindowsVirtualFileOffer(
    const FileClip& clip, std::vector<std::pair<UINT, std::string>> other_formats, Fetch fetch,
    Settings settings) :
    clip_(clip),
    other_formats_(std::move(other_formats)),
    fetch_(std::move(fetch)),
    settings_(std::move(settings)),
    offered_at_(std::chrono::steady_clock::now()),
    // (the CFSTR_ names are narrow or wide depending on UNICODE, so spelled out)
    descriptor_format_(RegisterClipboardFormatW(L"FileGroupDescriptorW")),
    contents_format_(RegisterClipboardFormatW(L"FileContents")),
    drop_effect_format_(RegisterClipboardFormatW(L"Preferred DropEffect")),
    fetched_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    for (const wchar_t* name : {L"LeapdeskOwnership",
                                L"ExcludeClipboardContentFromMonitorProcessing",
                                L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"}) {
        zero_formats_.push_back(RegisterClipboardFormatW(name));
    }
}

MSWindowsVirtualFileOffer::~MSWindowsVirtualFileOffer()
{
    if (settings_.released) {
        settings_.released();
    }
    if (fetched_event_ != nullptr) {
        CloseHandle(fetched_event_);
    }
}

std::string MSWindowsVirtualFileOffer::make_descriptor(const FileClip& clip)
{
    std::size_t count = clip.files.size();
    std::size_t size = sizeof(FILEGROUPDESCRIPTORW) +
                       (count > 0 ? count - 1 : 0) * sizeof(FILEDESCRIPTORW);
    std::string data(size, '\0');
    auto group = reinterpret_cast<FILEGROUPDESCRIPTORW*>(&data[0]);
    group->cItems = static_cast<UINT>(count);
    for (std::size_t i = 0; i < count; ++i) {
        const FileClip::File& file = clip.files[i];
        FILEDESCRIPTORW& descriptor = group->fgd[i];
        descriptor.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;
        descriptor.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        descriptor.nFileSizeHigh = static_cast<DWORD>(file.size >> 32);
        descriptor.nFileSizeLow = static_cast<DWORD>(file.size);
        descriptor.ftLastWriteTime.dwHighDateTime = static_cast<DWORD>(file.modified >> 32);
        descriptor.ftLastWriteTime.dwLowDateTime = static_cast<DWORD>(file.modified);
        wcsncpy_s(descriptor.cFileName, MAX_PATH, safe_file_name(file.name).c_str(), _TRUNCATE);
    }
    return data;
}

std::string MSWindowsVirtualFileOffer::clipboard_requester()
{
    HWND window = GetOpenClipboardWindow();
    if (window == nullptr) {
        return {};
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    std::string name;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            std::wstring image(path, size);
            std::wstring::size_type slash = image.find_last_of(L"\\/");
            name = win_wchar_to_utf8(
                (slash == std::wstring::npos ? image : image.substr(slash + 1)).c_str());
        }
        CloseHandle(process);
    }
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

bool MSWindowsVirtualFileOffer::refuses_paths_to(const std::string& program)
{
    // Explorer reads the paths the moment files land on the clipboard, and
    // when it gets none it uses the virtual files, fetching them only to
    // paste.  a program that can't be named may be Explorer too
    return program.empty() || program == "explorer.exe";
}

std::vector<FORMATETC> MSWindowsVirtualFileOffer::formats() const
{
    std::vector<FORMATETC> result;
    result.push_back(format_of(descriptor_format_, TYMED_HGLOBAL));
    result.push_back(format_of(contents_format_, TYMED_ISTREAM | TYMED_HGLOBAL));
    result.push_back(format_of(drop_effect_format_, TYMED_HGLOBAL));
    result.push_back(format_of(CF_HDROP, TYMED_HGLOBAL));
    for (UINT zero : zero_formats_) {
        result.push_back(format_of(zero, TYMED_HGLOBAL));
    }
    for (const auto& other : other_formats_) {
        result.push_back(format_of(other.first, TYMED_HGLOBAL));
    }
    return result;
}

HRESULT MSWindowsVirtualFileOffer::QueryInterface(REFIID iid, void** object)
{
    if (object == nullptr) {
        return E_POINTER;
    }
    if (iid == IID_IUnknown || iid == IID_IDataObject) {
        *object = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG MSWindowsVirtualFileOffer::AddRef()
{
    return ++references_;
}

ULONG MSWindowsVirtualFileOffer::Release()
{
    ULONG left = --references_;
    if (left == 0) {
        delete this;
    }
    return left;
}

HRESULT MSWindowsVirtualFileOffer::GetData(FORMATETC* format, STGMEDIUM* medium)
{
    if (format == nullptr || medium == nullptr) {
        return E_INVALIDARG;
    }
    std::memset(medium, 0, sizeof(*medium));
    if ((format->dwAspect & DVASPECT_CONTENT) == 0) {
        return DV_E_DVASPECT;
    }
    if (format->cfFormat == contents_format_) {
        return get_contents(format->lindex, format->tymed, medium);
    }
    if ((format->tymed & TYMED_HGLOBAL) == 0) {
        return DV_E_TYMED;
    }
    if (format->cfFormat == CF_HDROP) {
        return get_paths(medium);
    }

    std::string bytes;
    if (format->cfFormat == descriptor_format_) {
        bytes = make_descriptor(clip_);
    }
    else if (format->cfFormat == drop_effect_format_) {
        bytes = dword_bytes(DROPEFFECT_COPY);
    }
    else if (std::find(zero_formats_.begin(), zero_formats_.end(), format->cfFormat) !=
             zero_formats_.end()) {
        bytes = dword_bytes(0);
    }
    else {
        auto other = std::find_if(other_formats_.begin(), other_formats_.end(),
                                  [format](const auto& entry) {
                                      return entry.first == format->cfFormat;
                                  });
        if (other == other_formats_.end()) {
            return DV_E_FORMATETC;
        }
        bytes = other->second;
    }
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = make_global(bytes);
    return medium->hGlobal != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT MSWindowsVirtualFileOffer::get_contents(LONG index, DWORD tymed, STGMEDIUM* medium)
{
    if (index < 0 || static_cast<std::size_t>(index) >= clip_.files.size()) {
        return DV_E_LINDEX;
    }
    if ((tymed & (TYMED_ISTREAM | TYMED_HGLOBAL)) == 0) {
        return DV_E_TYMED;
    }
    std::string failure;
    if (!ensure_fetched(failure)) {
        return paste_failed(failure);
    }
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = paths_[index];
    }

    if ((tymed & TYMED_ISTREAM) != 0) {
        IStream* stream = nullptr;
        HRESULT result = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                                                FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
        if (FAILED(result)) {
            LOG_NOTE("couldn't open the pasted file %s: 0x%08lx",
                     win_wchar_to_utf8(path.c_str()).c_str(), static_cast<unsigned long>(result));
            return result;
        }
        medium->tymed = TYMED_ISTREAM;
        medium->pstm = stream;
        return S_OK;
    }

    // a block of memory, for programs that can't take a stream; not for big files
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
        info.nFileSizeHigh != 0 || info.nFileSizeLow > 256u * 1024 * 1024) {
        return DV_E_TYMED;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    std::string bytes(info.nFileSizeLow, '\0');
    DWORD read = 0;
    bool ok = bytes.empty() ||
              (ReadFile(file, &bytes[0], info.nFileSizeLow, &read, nullptr) && read == bytes.size());
    CloseHandle(file);
    if (!ok) {
        return E_FAIL;
    }
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = make_global(bytes);
    return medium->hGlobal != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT MSWindowsVirtualFileOffer::get_paths(STGMEDIUM* medium)
{
    std::string program = clipboard_requester();
    bool refused = settings_.refuse_paths ? settings_.refuse_paths(program)
                                          : refuses_paths_to(program);
    if (refused) {
        LOG_DEBUG("not handing over the files' paths to %s; it can use the files themselves",
                  program.empty() ? "a program that can't be named" : program.c_str());
        return DV_E_FORMATETC;
    }
    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - offered_at_);
    if (since < settings_.settle_time) {
        LOG_DEBUG("not handing over the files' paths to %s %lld ms after offering them",
                  program.c_str(), static_cast<long long>(since.count()));
        return DV_E_FORMATETC;
    }

    std::string failure;
    if (!ensure_fetched(failure)) {
        return paste_failed(failure);
    }
    std::vector<std::wstring> paths;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paths = paths_;
    }
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = MSWindowsFilePaste::make_drop(paths);
    return medium->hGlobal != nullptr ? S_OK : E_OUTOFMEMORY;
}

bool MSWindowsVirtualFileOffer::ensure_fetched(std::string& failure)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fetched_) {
            return true;
        }
        if (failed_ && !fetching_ &&
            std::chrono::steady_clock::now() - failed_at_ < settings_.failure_hold) {
            failure = failure_;
            return false;
        }
        if (!fetching_) {
            fetching_ = true;
            ResetEvent(fetched_event_);
            ++fetches_;
            // the thread keeps this alive until the fetch ends
            AddRef();
            std::thread([this]() {
                std::vector<std::wstring> paths;
                std::string why;
                bool ok = fetch_(paths, why);
                if (ok && paths.size() != clip_.files.size()) {
                    ok = false;
                    why = "the wrong number of files arrived";
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    fetching_ = false;
                    fetched_ = ok;
                    failed_ = !ok;
                    failed_at_ = std::chrono::steady_clock::now();
                    paths_ = ok ? paths : std::vector<std::wstring>();
                    failure_ = why;
                }
                SetEvent(fetched_event_);
                Release();
            }).detach();
        }
    }

    // the application pasting waits; other calls (asking for the next file,
    // say) are let through meanwhile, as COM expects of this thread
    DWORD index = 0;
    HRESULT waited = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS |
                                              COWAIT_DISPATCH_WINDOW_MESSAGES,
                                              INFINITE, 1, &fetched_event_, &index);
    std::lock_guard<std::mutex> lock(mutex_);
    if (fetched_) {
        return true;
    }
    failure = FAILED(waited) ? "waiting for the files failed" : failure_;
    return false;
}

HRESULT MSWindowsVirtualFileOffer::GetDataHere(FORMATETC*, STGMEDIUM*)
{
    return E_NOTIMPL;
}

HRESULT MSWindowsVirtualFileOffer::QueryGetData(FORMATETC* format)
{
    if (format == nullptr) {
        return E_INVALIDARG;
    }
    if ((format->dwAspect & DVASPECT_CONTENT) == 0) {
        return DV_E_DVASPECT;
    }
    for (const auto& offered : formats()) {
        if (offered.cfFormat == format->cfFormat) {
            return (offered.tymed & format->tymed) != 0 ? S_OK : DV_E_TYMED;
        }
    }
    return DV_E_FORMATETC;
}

HRESULT MSWindowsVirtualFileOffer::GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out)
{
    if (in == nullptr || out == nullptr) {
        return E_INVALIDARG;
    }
    *out = *in;
    out->ptd = nullptr;
    return DATA_S_SAMEFORMATETC;
}

HRESULT MSWindowsVirtualFileOffer::SetData(FORMATETC*, STGMEDIUM* medium, BOOL release)
{
    // the shell reports how the paste went (Performed DropEffect and the
    // like); nothing here needs it
    if (release && medium != nullptr) {
        ReleaseStgMedium(medium);
    }
    return S_OK;
}

HRESULT MSWindowsVirtualFileOffer::EnumFormatEtc(DWORD direction, IEnumFORMATETC** formats_out)
{
    if (formats_out == nullptr) {
        return E_POINTER;
    }
    if (direction != DATADIR_GET) {
        *formats_out = nullptr;
        return E_NOTIMPL;
    }
    std::vector<FORMATETC> offered = formats();
    return SHCreateStdEnumFmtEtc(static_cast<UINT>(offered.size()), offered.data(), formats_out);
}

HRESULT MSWindowsVirtualFileOffer::DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

HRESULT MSWindowsVirtualFileOffer::DUnadvise(DWORD)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

HRESULT MSWindowsVirtualFileOffer::EnumDAdvise(IEnumSTATDATA**)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

} // namespace inputleap
