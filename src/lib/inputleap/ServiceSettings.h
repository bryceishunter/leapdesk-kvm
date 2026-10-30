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

#include <functional>
#include <string>
#include <vector>

namespace inputleap {

/*!
What the Leapdesk service runs on Windows, as the installer stores it in
HKLM\SOFTWARE\Leapdesk.  Only administrators can write there, and the service
builds the command line itself from these values and the programs in its own
folder, so nothing a normal user or program sends it can change what runs
elevated.
*/
struct ServiceSettings {
    //! Reads one setting by name, giving "" when it isn't set
    using Reader = std::function<std::string(const std::string& name)>;
    //! Stores one setting by name
    using Writer = std::function<void(const std::string& name, const std::string& value)>;

    std::string role;               //!< "Server" or "Client"; empty until one is chosen
    std::string screen_name;        //!< --name
    std::string server_address;     //!< the client's server, host or host:port
    std::string log_level = "NOTE"; //!< --debug
    std::string profile_dir;        //!< the user's folder for certificates, configuration and logs
    bool elevate = true;            //!< run with the service's own rights, for UAC prompts

    //! Reads the values Role, ScreenName, ServerAddress, LogLevel, ProfileDir
    //! and Elevate.  Role and LogLevel are normalised to the case used here.
    static ServiceSettings read(const Reader& setting);

    //! Stores the values read() reads
    void write(const Writer& setting) const;

    bool is_server() const { return role == "Server"; }

    //! Why these settings can't be run, or "" when they can
    std::string problem() const;

    //! The command line that runs the role with the programs in \p program_dir.
    //! Only call this when problem() is empty.
    std::string command(const std::string& program_dir) const;

    //! The arguments that make leapdesk-daemon.exe /configure store these
    //! settings, quoted for a command line
    std::string configure_arguments() const;

    //! Changes these settings as those arguments say (--role, --name, --server,
    //! --log-level, --profile-dir).  Returns why it couldn't, or "".
    std::string apply_arguments(const std::vector<std::string>& arguments);
};

} // namespace inputleap
