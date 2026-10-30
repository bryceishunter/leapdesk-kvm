/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
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

#include "arch/Arch.h"
#include "base/Fwd.h"
#include "ipc/IpcServer.h"

#include <string>

namespace inputleap {

class IpcLogOutputter;
class MSWindowsWatchdog;

class DaemonApp {

public:
    DaemonApp();
    virtual ~DaemonApp();
    int run(int argc, char** argv);
    void mainLoop(bool daemonized);

private:
    void daemonize();
    void foregroundError(const char* message);
    std::string logFilename();
    // the folder this program was started from, which holds the server and client
    static std::string program_dir();
    // gives the watchdog the command that HKLM\SOFTWARE\Leapdesk sets up, if any
    void start_from_settings();
    void handle_ipc_message(const Event& event);

public:
    static DaemonApp* s_instance;

    MSWindowsWatchdog* m_watchdog;

private:
    IpcServer* m_ipcServer;
    IpcLogOutputter* m_ipcLogOutputter;
    IEventQueue* m_events;
    FileLogOutputter* m_fileLogOutputter;
};

#define LOG_FILENAME "leapdesk-daemon.log"

} // namespace inputleap
