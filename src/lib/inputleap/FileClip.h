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

#include <cstdint>
#include <string>
#include <vector>

namespace inputleap {

/*!
Files copied on one screen, as the other screens see them.  A FileClip travels
in the clipboard as IClipboard::kFiles and holds only names and sizes: the files
stay on the screen that copied them until a paste on another screen asks that
screen to send them.
*/
struct FileClip {
    //! The most files one copy can hold
    static const std::size_t kMaxFiles = 4096;

    struct File {
        std::string name;               //!< file name without a directory, UTF-8
        std::uint64_t size = 0;         //!< in bytes
        std::uint64_t modified = 0;     //!< last write time, in the source's units
    };

    //! Identifies this copy.  The source checks a paste's id against the files
    //! on its own clipboard before sending anything, so a paste can only fetch
    //! the files that are copied there now.
    std::string id;
    std::vector<File> files;

    //! Encodes the clip as the data of IClipboard::kFiles
    std::string marshall() const;

    //! Decodes \p data, which came over the network, into \p clip.  Returns
    //! false, leaving \p clip unspecified, if it isn't a valid clip.
    static bool unmarshall(const std::string& data, FileClip& clip);

    //! The id of a copy of the files at \p paths, whose sizes and times are in
    //! \p files.  Copying the same unchanged files again gives the same id.
    static std::string make_id(const std::vector<std::string>& paths,
                               const std::vector<File>& files);

    //! The name file \p index is sent under for paste \p request, so that the
    //! pasting screen can recognise it when it arrives
    std::string transfer_name(const std::string& request, std::size_t index) const;

    //! How transfer_name() starts, whatever the file is called
    static std::string transfer_prefix(const std::string& request, std::size_t index);
};

//! How a file paste is going, as the screen sending the files reports it
enum class FilePasteState : std::uint8_t {
    SENDING = 1,    //!< the source has started sending
    SENT = 2,       //!< every file has been handed over; nothing more will come
    FAILED = 3,     //!< nothing more will come; the detail says why
};

//! A paste of a FileClip, asking the screen that copied the files to send them
struct FilePasteRequest {
    std::string request;    //!< chosen by the pasting screen to name this paste
    std::string file_id;    //!< FileClip::id of the files to paste
    std::string address;    //!< where to send them: the pasting screen's Tailscale address
};

//! Progress of a FilePasteRequest, reported back to the pasting screen
struct FilePasteStatus {
    std::string request;
    FilePasteState state = FilePasteState::FAILED;
    std::string detail;
};

//! The detail of a SENDING status that says how far the files have got:
//! bytes sent so far, and in all.  Progress goes out as SENDING rather than a
//! state of its own because a screen running an older version takes SENDING
//! as ordinary news, where it would drop the connection over a new state.
std::string file_paste_progress(std::uint64_t sent, std::uint64_t total);

//! Reads a file_paste_progress() detail; false if \p detail isn't one
bool parse_file_paste_progress(const std::string& detail, std::uint64_t& sent,
                               std::uint64_t& total);

//! Whether \p request can name a paste: it ends up in file names
bool is_valid_paste_request(const std::string& request);

//! Whether \p address can be a Tailscale address or machine name: it ends up
//! on a command line
bool is_valid_paste_address(const std::string& address);

} // namespace inputleap
