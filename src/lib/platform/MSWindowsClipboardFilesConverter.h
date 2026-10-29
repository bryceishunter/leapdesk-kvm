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

#include "platform/MSWindowsClipboard.h"
#include "inputleap/FileClip.h"

#include <string>
#include <vector>

namespace inputleap {

//! Convert files copied in Explorer (CF_HDROP) to a FileClip
/*!
Only describes the files; they are sent when another screen pastes them.
*/
class MSWindowsClipboardFilesConverter : public IMSWindowsClipboardConverter {
public:
    // IMSWindowsClipboardConverter overrides
    IClipboard::EFormat getFormat() const override;
    UINT getWin32Format() const override;
    HANDLE fromIClipboard(const std::string&) const override;
    std::string toIClipboard(HANDLE) const override;

    //! The paths in CF_HDROP data
    static std::vector<std::wstring> get_paths(HANDLE drop);

    //! Reads the copied files' paths through the clipboard's OLE data object.
    //! Only works in a process running as the user who copied them, so the
    //! service uses read_clipboard_files() instead.  Returns true if the
    //! clipboard holds files, even if reading their paths then failed.  Says
    //! what went wrong, if anything, in \p why.
    static bool clipboard_file_paths(std::vector<std::wstring>& paths,
                                     std::string* why = nullptr);

    //! The files currently on the clipboard
    struct ClipboardFiles {
        bool present = false;               //!< the clipboard holds files
        bool ok = false;                    //!< clip and paths are valid to send
        bool answered = false;              //!< the clipboard could be read at all
        std::string problem;                //!< why not, when present but not ok
        FileClip clip;
        std::vector<std::wstring> paths;    //!< full paths, for sending
    };

    //! Whether the clipboard might hold files.  Cheap (no OLE), so callers
    //! gate the heavier read on it and do no work for plain text copies.
    static bool clipboard_maybe_has_files();

    //! Reads the files on the clipboard.  When this runs as SYSTEM (the
    //! service) it runs a helper as the signed-in user, because a service
    //! can't read an application's OLE clipboard; otherwise it reads directly.
    static ClipboardFiles read_clipboard_files();

    //! The body of the "--leapdesk-clipboard-files" helper: prints the
    //! clipboard's files for its parent, running as the user.  Returns an
    //! exit code.
    static int dump_to_stdout();

    //! Describes the files at \p paths in \p clip, looking at them as the
    //! signed-in user.  Returns false if any of them isn't a file that can
    //! be read, since only files can be sent, and says why in \p problem.
    static bool make_file_clip(const std::vector<std::wstring>& paths, FileClip& clip,
                               std::string* problem = nullptr);

    //! Logs the copy of the files at \p paths: described by \p clip, or not
    //! offered for \p problem if \p clip is null.  \p copying is true when
    //! the copy is noted as it happens, false when the clipboard is read.
    //! Each copy is logged only once.
    static void report_copy(const std::vector<std::wstring>& paths, const FileClip* clip,
                            const std::string& problem, bool copying);

private:
    //! read_clipboard_files() without reusing an earlier answer
    static ClipboardFiles read_clipboard_files_now();
};

} // namespace inputleap
