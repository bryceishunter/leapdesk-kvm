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

#include <string>

namespace inputleap {

//! The paste helper
/*!
A copy of the server or client program, run as the signed-in user for the
whole session, that offers files copied on another screen to the user's
applications as virtual files (MSWindowsVirtualFileOffer).  The service, which
runs Leapdesk as SYSTEM, can't do that itself: applications can't read an OLE
clipboard object it puts up.  So it starts this, sends it each offer over a
named pipe, and is asked in return to have the files sent when an application
pastes them.  While they come, it shows how the paste is going
(MSWindowsPasteProgress), with a button to cancel it.

Messages, service to helper: "offer" (generation, marshalled clipboard),
"supersede" (generation), "status" (request, state, detail), "connected"
("1"/"0").  Helper to service: "hello" (process id), "placed" (generation,
clipboard sequence number), "request" (request, file id, address),
"cancelled" (request), "log" (level, text).
*/
class MSWindowsPasteAgent {
public:
    struct Options {
        std::wstring pipe;              //!< the service's pipe
        std::wstring staging_folder;    //!< where pasted files are kept
        std::string log_level = "NOTE";
        // for tests: where received files arrive, this machine's address,
        // how long CF_HDROP is held back, and whether it's refused to all
        std::wstring received_folder;
        std::string own_address;
        int settle_ms = -1;
        bool refuse_all_paths = false;
    };

    static constexpr const char* kOption = "--leapdesk-paste-agent";

    //! The command-line arguments that run the helper with \p options
    static std::wstring arguments(const Options& options);

    //! Runs the helper if the command line asks for it, before anything else
    //! starts, and returns true with its exit code in \p exit_code
    static bool run_if_asked(int argc, char** argv, int& exit_code);

    //! Runs until the service goes away; returns an exit code
    static int run(const Options& options);
};

} // namespace inputleap
