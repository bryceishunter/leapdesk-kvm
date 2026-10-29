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

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>

namespace inputleap {

//! Whether this process runs as SYSTEM, as it does under the Leapdesk service
bool running_as_system();

//! Switches on privilege \p name, such as SE_TCB_NAME, if this process holds it
bool enable_privilege(const char* name);

//! Runs \p command as the user signed in to this session, on the interactive
//! desktop, with its standard output captured into \p output and its exit code
//! in \p exit_code.  When this doesn't run as SYSTEM, runs it as the current
//! user.  Only the output pipe is inherited, not this process's other handles.
//! Returns false if the program couldn't be started or timed out.
bool run_as_session_user(const std::wstring& command, DWORD& exit_code, std::string& output,
                         DWORD timeout_ms = 15000);

//! The token of the user signed in to this process's session
/*!
Only when this runs as SYSTEM, as it does under the Leapdesk service; the
caller closes it.  Null otherwise, since this then runs as that user already,
or if it couldn't be had, which is logged.
*/
HANDLE session_user_token();

//! The name of the user signed in to this process's session, as DOMAIN\\user
/*!
Only when this runs as SYSTEM; empty otherwise, or if nobody is signed in.
*/
std::string session_user_name();

//! Acts as the user signed in to this session on this thread while it exists
/*!
Does nothing unless this runs as SYSTEM.  Work on files in the user's
folders is done this way, so that a link the user put there can't turn it
into something only SYSTEM may do.
*/
class ImpersonateSessionUser {
public:
    ImpersonateSessionUser();
    ~ImpersonateSessionUser();
    ImpersonateSessionUser(const ImpersonateSessionUser&) = delete;
    ImpersonateSessionUser& operator=(const ImpersonateSessionUser&) = delete;

    //! Whether this runs as SYSTEM but couldn't act as the user
    bool failed() const { return failed_; }

private:
    bool impersonating_ = false;
    bool failed_ = false;
};

} // namespace inputleap
