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

#pragma once

#include "inputleap/FileClip.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ole2.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace inputleap {

//! Files copied on another screen, offered to applications as virtual files
/*!
An OLE data object for the clipboard.  It describes the files at once
(FileGroupDescriptorW: names, sizes, times), but fetches them only when an
application asks for their contents (FileContents).  Explorer does that when it
pastes, and not when it merely looks at the clipboard, so a file stays where
it was copied until it is really pasted.  The first request for contents
fetches all of the files; each is then handed over as a stream on the file
that arrived.  Explorer shows its own progress meanwhile.

Programs that only take real paths can still ask for CF_HDROP, which fetches
the files too.  Explorer is refused it, so that it falls back to the virtual
files; so are requests in the first moments after the offer, when clipboard
watchers look at every new item.

It must be created, and put on the clipboard with OleSetClipboard(), by a
process running as the user who pastes, not by a service running as SYSTEM.
*/
class MSWindowsVirtualFileOffer : public IDataObject {
public:
    //! Fetches the files, blocking until they have all arrived or one has
    //! failed: their local paths, in the order of the FileClip, or false and why
    using Fetch = std::function<bool(std::vector<std::wstring>& paths, std::string& failure)>;

    struct Settings {
        //! How long after the offer requests for CF_HDROP are refused
        std::chrono::milliseconds settle_time{std::chrono::seconds(1)};
        //! How long after a fetch fails further requests fail at once, so that
        //! the rest of that paste doesn't fetch again
        std::chrono::milliseconds failure_hold{std::chrono::seconds(3)};
        //! Which programs (executable names, lower case) are refused CF_HDROP;
        //! refuses_paths_to() if not set
        std::function<bool(const std::string& program)> refuse_paths;
        //! Called when the data object goes away (the clipboard changed)
        std::function<void()> released;
    };

    //! A new offer of the files in \p clip, with \p other_formats (the
    //! clipboard's other data, as Win32 format and bytes).  Its reference
    //! count is 1.
    static MSWindowsVirtualFileOffer* create(const FileClip& clip,
                                             std::vector<std::pair<UINT, std::string>> other_formats,
                                             Fetch fetch, Settings settings);

    //! The FileGroupDescriptorW describing \p clip
    static std::string make_descriptor(const FileClip& clip);

    //! The executable name of the program that has the clipboard open now
    //! (the one asking for data during a render), lower case, or empty
    static std::string clipboard_requester();

    //! Whether \p program (an executable name, lower case) is refused CF_HDROP
    static bool refuses_paths_to(const std::string& program);

    //! How many times the files were fetched (for tests)
    int fetches() const { return fetches_; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDataObject
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override;
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* format, STGMEDIUM* medium) override;
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override;
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override;
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override;
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** formats) override;
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override;
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override;

private:
    MSWindowsVirtualFileOffer(const FileClip& clip,
                              std::vector<std::pair<UINT, std::string>> other_formats,
                              Fetch fetch, Settings settings);
    ~MSWindowsVirtualFileOffer();

    // the formats offered, in order of preference
    std::vector<FORMATETC> formats() const;
    // fetches the files if they haven't been, letting COM calls through while
    // waiting; false and why if that failed
    bool ensure_fetched(std::string& failure);
    HRESULT get_contents(LONG index, DWORD tymed, STGMEDIUM* medium);
    HRESULT get_paths(STGMEDIUM* medium);

    std::atomic<ULONG> references_{1};
    FileClip clip_;
    std::vector<std::pair<UINT, std::string>> other_formats_;
    Fetch fetch_;
    Settings settings_;
    std::chrono::steady_clock::time_point offered_at_;

    UINT descriptor_format_;
    UINT contents_format_;
    UINT drop_effect_format_;
    // formats holding a zero DWORD: the Leapdesk marker, and the ones that keep
    // clipboard history and the cloud clipboard out of it
    std::vector<UINT> zero_formats_;

    // the fetch: one at a time, done on a thread of its own
    std::mutex mutex_;
    HANDLE fetched_event_ = nullptr;    // set when the fetch in progress ends
    bool fetching_ = false;
    bool fetched_ = false;
    // the last fetch failed, and when: for a moment, the rest of that paste
    // fails at once rather than fetch again
    bool failed_ = false;
    std::chrono::steady_clock::time_point failed_at_;
    std::vector<std::wstring> paths_;
    std::string failure_;
    std::atomic<int> fetches_{0};
};

} // namespace inputleap
