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

#include "inputleap/ServiceSettings.h"

#include <algorithm>
#include <cctype>

namespace inputleap {

namespace {

const char* const kLogLevels[] = { "FATAL", "ERROR", "WARNING", "NOTE", "INFO", "DEBUG", "DEBUG1", "DEBUG2" };

std::string trimmed(const std::string& text)
{
    const char* space = " \t\r\n";
    auto first = text.find_first_not_of(space);
    if (first == std::string::npos)
        return "";
    return text.substr(first, text.find_last_not_of(space) - first + 1);
}

std::string upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

bool is_log_level(const std::string& level)
{
    return std::find_if(std::begin(kLogLevels), std::end(kLogLevels),
                        [&](const char* name) { return level == name; }) != std::end(kLogLevels);
}

// as the installer checks them
bool is_screen_name(const std::string& name)
{
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
    });
}

// a host name or address with an optional port, [bracketed] for IPv6.  It ends up as a
// bare argument, so it may not start with a dash and be taken for an option.
bool is_server_address(const std::string& address)
{
    return !address.empty() && address[0] != '-' &&
        std::all_of(address.begin(), address.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ':' ||
                   c == '[' || c == ']' || c == '%';
        });
}

// C:\... or \\server\share\...
bool is_full_path(const std::string& path)
{
    bool drive = path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
                 path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    bool unc = path.size() >= 3 && path[0] == '\\' && path[1] == '\\';
    return (drive || unc) && std::none_of(path.begin(), path.end(), [](unsigned char c) {
        return c < 0x20 || c == '"';
    });
}

std::string without_trailing_separators(std::string path)
{
    while (path.size() > 3 && (path.back() == '\\' || path.back() == '/'))
        path.pop_back();
    return path;
}

// Quotes an argument the way the C runtime splits command lines: backslashes before
// the closing quote are doubled so they don't escape it.
std::string quoted(const std::string& argument)
{
    std::size_t backslashes = 0;
    while (backslashes < argument.size() && argument[argument.size() - 1 - backslashes] == '\\')
        ++backslashes;
    return "\"" + argument + std::string(backslashes, '\\') + "\"";
}

} // namespace

ServiceSettings ServiceSettings::read(const Reader& setting)
{
    ServiceSettings settings;

    settings.role = trimmed(setting("Role"));
    if (upper(settings.role) == "SERVER")
        settings.role = "Server";
    else if (upper(settings.role) == "CLIENT")
        settings.role = "Client";

    settings.screen_name = trimmed(setting("ScreenName"));
    settings.server_address = trimmed(setting("ServerAddress"));
    std::string level = upper(trimmed(setting("LogLevel")));
    if (!level.empty())
        settings.log_level = level;
    settings.profile_dir = trimmed(setting("ProfileDir"));
    settings.elevate = trimmed(setting("Elevate")) != "0";
    return settings;
}

std::string ServiceSettings::problem() const
{
    if (role.empty())
        return "no role is set";
    if (role != "Server" && role != "Client")
        return "the role \"" + role + "\" is neither Server nor Client";
    if (screen_name.empty())
        return "no screen name is set";
    if (!is_screen_name(screen_name))
        return "the screen name \"" + screen_name +
               "\" may only use letters, digits, hyphens, underscores and dots";
    if (!is_server()) {
        if (server_address.empty())
            return "no server address is set";
        if (!is_server_address(server_address))
            return "the server address \"" + server_address +
                   "\" isn't a host name or address with an optional port";
    }
    if (!is_log_level(log_level))
        return "the log level \"" + log_level +
               "\" isn't one of FATAL, ERROR, WARNING, NOTE, INFO, DEBUG, DEBUG1 or DEBUG2";
    if (profile_dir.empty())
        return "no profile folder is set";
    if (!is_full_path(profile_dir))
        return "the profile folder \"" + profile_dir + "\" isn't a full path";
    return "";
}

std::string ServiceSettings::command(const std::string& program_dir) const
{
    const std::string programs = without_trailing_separators(program_dir);
    const std::string profile = without_trailing_separators(profile_dir);
    const std::string program = is_server() ? "server" : "client";

    std::string command = quoted(programs + "\\leapdesk-" + program + ".exe") +
        " -f --no-tray --ipc --debug " + log_level +
        " --name " + screen_name +
        " --profile-dir " + quoted(profile) +
        " --log " + quoted(profile + "\\" + program + ".log");
    if (is_server())
        return command + " -c " + quoted(profile + "\\leapdesk.sgc");
    // the server address has to be the last argument
    return command + " " + server_address;
}

} // namespace inputleap
