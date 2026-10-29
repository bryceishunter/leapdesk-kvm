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

#include <string>
#include <vector>

namespace inputleap {

//! Reads the files copied in Explorer (CF_HDROP) as a FileClip
/*!
Only describes the files; they are sent when another screen pastes them.

The service runs Leapdesk as SYSTEM, which can't read the files an application
such as Explorer puts on the clipboard: it sees only an opaque OLE wrapper,
and acting as the user from inside SYSTEM doesn't help.  So under SYSTEM a
copy of the program is started as the signed-in user to read them (the
"--leapdesk-clipboard-files" helper); otherwise they are read directly.
*/
class MSWindowsClipboardFiles {
public:
    //! The files currently on the clipboard
    struct Files {
        bool present = false;               //!< the clipboard holds files
        bool ok = false;                    //!< clip and paths are valid to send
        bool answered = false;              //!< the clipboard could be read at all
        std::string problem;                //!< why not, when present but not ok
        FileClip clip;
        std::vector<std::wstring> paths;    //!< full paths, for sending
    };

    //! The command-line option that runs the helper
    static constexpr const char* kHelperOption = "--leapdesk-clipboard-files";
    //! Added to it to have the helper note what it did in its own log
    static constexpr const char* kHelperLogOption = "--leapdesk-clipboard-files-log";

    //! Whether the clipboard might hold files.  Cheap (no OLE), so callers
    //! gate the heavier read on it and do no work for plain text copies.
    static bool clipboard_maybe_has_files();

    //! Reads the files on the clipboard, starting the helper if this runs as
    //! SYSTEM.  Reuses the last answer until the clipboard changes.
    static Files read_clipboard_files();

    //! The helper's body: prints the clipboard's files for its parent, running
    //! as the user, and with \p keep_log notes what it did in
    //! clipboard-helper.log in the user's Leapdesk folder.  Returns an exit code.
    static int dump_to_stdout(bool keep_log);

    //! Runs the helper if the command line asks for it, before anything else
    //! starts, and returns true with its exit code in \p exit_code
    static bool run_helper_if_asked(int argc, char** argv, int& exit_code);

    //! The paths in CF_HDROP data
    static std::vector<std::wstring> get_paths(HANDLE drop);

    //! Reads the copied files' paths through the clipboard's OLE data object.
    //! Only works in a process running as the user who copied them.  Returns
    //! true if the clipboard holds files, even if reading their paths then
    //! failed.  Says what went wrong, if anything, in \p why.
    static bool clipboard_file_paths(std::vector<std::wstring>& paths,
                                     std::string* why = nullptr);

    //! Describes the files at \p paths in \p clip, looking at them as the
    //! signed-in user.  Returns false if any of them isn't a file that can
    //! be read, since only files can be sent, and says why in \p problem.
    static bool make_file_clip(const std::vector<std::wstring>& paths, FileClip& clip,
                               std::string* problem = nullptr);

    //! Logs a copy of files: described by \p clip, or not offered for
    //! \p problem if \p clip is null.  Each copy is logged once.
    static void report_copy(const FileClip* clip, const std::string& problem);

private:
    //! read_clipboard_files() without reusing an earlier answer
    static Files read_clipboard_files_now();
};

} // namespace inputleap
