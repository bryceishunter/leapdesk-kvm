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

#include <gtest/gtest.h>

#include <map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace inputleap {

namespace {

const char kPrograms[] = "C:\\Program Files\\Leapdesk";
const char kProfile[] = "C:\\Users\\A B\\AppData\\Local\\Leapdesk";

ServiceSettings read(const std::map<std::string, std::string>& values)
{
    return ServiceSettings::read([&](const std::string& name) {
        auto found = values.find(name);
        return found == values.end() ? std::string() : found->second;
    });
}

ServiceSettings server()
{
    return read({{"Role", "Server"}, {"ScreenName", "DESKTOP-GP522K1"}, {"ProfileDir", kProfile}});
}

ServiceSettings client()
{
    return read({{"Role", "Client"}, {"ScreenName", "surfacestudio"},
                 {"ServerAddress", "100.111.152.25:24800"}, {"ProfileDir", kProfile}});
}

} // namespace

TEST(ServiceSettingsTests, readsAndNormalisesTheValues)
{
    auto settings = read({{"Role", " server "}, {"ScreenName", "DESKTOP-GP522K1"},
                          {"LogLevel", "debug"}, {"ProfileDir", kProfile}});

    EXPECT_EQ(settings.role, "Server");
    EXPECT_TRUE(settings.is_server());
    EXPECT_EQ(settings.screen_name, "DESKTOP-GP522K1");
    EXPECT_EQ(settings.log_level, "DEBUG");
    EXPECT_EQ(settings.profile_dir, kProfile);
    EXPECT_TRUE(settings.elevate);
}

TEST(ServiceSettingsTests, logsAtNoteAndElevatesUnlessTold)
{
    auto settings = client();
    EXPECT_EQ(settings.log_level, "NOTE");
    EXPECT_TRUE(settings.elevate);

    EXPECT_TRUE(read({{"Elevate", "1"}}).elevate);
    EXPECT_FALSE(read({{"Elevate", "0"}}).elevate);
}

TEST(ServiceSettingsTests, serverCommandIsTheOneTheInstallerUsedToWrite)
{
    auto settings = server();
    ASSERT_EQ(settings.problem(), "");
    EXPECT_EQ(settings.command(kPrograms),
              "\"C:\\Program Files\\Leapdesk\\leapdesk-server.exe\" -f --no-tray --ipc --debug NOTE"
              " --name DESKTOP-GP522K1 --profile-dir \"C:\\Users\\A B\\AppData\\Local\\Leapdesk\""
              " --log \"C:\\Users\\A B\\AppData\\Local\\Leapdesk\\server.log\""
              " -c \"C:\\Users\\A B\\AppData\\Local\\Leapdesk\\leapdesk.sgc\"");
}

TEST(ServiceSettingsTests, clientCommandEndsWithTheServer)
{
    auto settings = client();
    ASSERT_EQ(settings.problem(), "");
    EXPECT_EQ(settings.command(kPrograms),
              "\"C:\\Program Files\\Leapdesk\\leapdesk-client.exe\" -f --no-tray --ipc --debug NOTE"
              " --name surfacestudio --profile-dir \"C:\\Users\\A B\\AppData\\Local\\Leapdesk\""
              " --log \"C:\\Users\\A B\\AppData\\Local\\Leapdesk\\client.log\" 100.111.152.25:24800");
}

TEST(ServiceSettingsTests, dropsTrailingSeparatorsFromFolders)
{
    auto settings = server();
    settings.profile_dir = "D:\\Profiles\\me\\";

    EXPECT_EQ(settings.command("C:\\Program Files\\Leapdesk\\"),
              "\"C:\\Program Files\\Leapdesk\\leapdesk-server.exe\" -f --no-tray --ipc --debug NOTE"
              " --name DESKTOP-GP522K1 --profile-dir \"D:\\Profiles\\me\""
              " --log \"D:\\Profiles\\me\\server.log\" -c \"D:\\Profiles\\me\\leapdesk.sgc\"");
}

TEST(ServiceSettingsTests, keepsTheQuoteAfterADriveRoot)
{
    auto settings = client();
    settings.profile_dir = "D:\\";

    // the backslash is doubled so it doesn't escape the closing quote
    EXPECT_NE(settings.command(kPrograms).find(" --profile-dir \"D:\\\\\" "), std::string::npos);
}

TEST(ServiceSettingsTests, needsARole)
{
    EXPECT_EQ(read({}).problem(), "no role is set");
    auto settings = server();
    settings.role = "Desktop";
    EXPECT_EQ(settings.problem(), "the role \"Desktop\" is neither Server nor Client");
}

TEST(ServiceSettingsTests, needsAPlainScreenName)
{
    auto settings = server();
    settings.screen_name = "";
    EXPECT_EQ(settings.problem(), "no screen name is set");

    for (const char* name : {"my pc", "pc\"", "--disable-crypto pc", "pc;"}) {
        settings.screen_name = name;
        EXPECT_NE(settings.problem(), "") << name;
    }
    settings.screen_name = "studio.local_2";
    EXPECT_EQ(settings.problem(), "");
}

TEST(ServiceSettingsTests, clientNeedsAServerAddress)
{
    auto settings = client();
    settings.server_address = "";
    EXPECT_EQ(settings.problem(), "no server address is set");

    for (const char* address : {"--disable-crypto", "-a", "host name", "host\"", "host;calc"}) {
        settings.server_address = address;
        EXPECT_NE(settings.problem(), "") << address;
    }
    for (const char* address : {"100.111.152.25", "desktop-gp522k1:24800", "[fd7a:115c::1]:24800"}) {
        settings.server_address = address;
        EXPECT_EQ(settings.problem(), "") << address;
    }
}

TEST(ServiceSettingsTests, serverNeedsNoServerAddress)
{
    EXPECT_EQ(server().server_address, "");
    EXPECT_EQ(server().problem(), "");
}

TEST(ServiceSettingsTests, needsAKnownLogLevel)
{
    auto settings = server();
    settings.log_level = "LOUD";
    EXPECT_NE(settings.problem().find("log level \"LOUD\""), std::string::npos);
    settings.log_level = "DEBUG2";
    EXPECT_EQ(settings.problem(), "");
}

TEST(ServiceSettingsTests, needsAFullProfileFolder)
{
    auto settings = client();
    settings.profile_dir = "";
    EXPECT_EQ(settings.problem(), "no profile folder is set");

    for (const char* folder : {"Leapdesk", "\\Leapdesk", "C:Leapdesk", "C:\\a\"b"}) {
        settings.profile_dir = folder;
        EXPECT_NE(settings.problem(), "") << folder;
    }
    settings.profile_dir = "\\\\nas\\home\\Leapdesk";
    EXPECT_EQ(settings.problem(), "");
}

#ifdef _WIN32
TEST(ServiceSettingsTests, windowsSplitsTheCommandIntoTheIntendedArguments)
{
    auto settings = client();
    settings.profile_dir = "D:\\";
    std::string command = settings.command(kPrograms);

    int count = 0;
    std::wstring wide(command.begin(), command.end());
    wchar_t** arguments = CommandLineToArgvW(wide.c_str(), &count);
    ASSERT_NE(arguments, nullptr);
    std::vector<std::wstring> split(arguments, arguments + count);
    LocalFree(arguments);

    std::vector<std::wstring> expected = {
        L"C:\\Program Files\\Leapdesk\\leapdesk-client.exe", L"-f", L"--no-tray", L"--ipc",
        L"--debug", L"NOTE", L"--name", L"surfacestudio", L"--profile-dir", L"D:\\",
        L"--log", L"D:\\\\client.log", L"100.111.152.25:24800"};
    EXPECT_EQ(split, expected);
}
#endif

} // namespace inputleap
