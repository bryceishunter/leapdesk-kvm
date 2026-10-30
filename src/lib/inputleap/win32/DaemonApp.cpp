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

#include "inputleap/win32/DaemonApp.h"

#include "inputleap/App.h"
#include "inputleap/ServiceSettings.h"
#include "ipc/IpcClientProxy.h"
#include "ipc/IpcMessage.h"
#include "ipc/IpcLogOutputter.h"
#include "net/SocketMultiplexer.h"
#include "arch/XArch.h"
#include "base/Log.h"
#include "base/EventQueue.h"
#include "base/log_outputters.h"
#include "base/Log.h"
#include "common/DataDirectories.h"
#include "io/filesystem.h"

#include "arch/win32/ArchMiscWindows.h"
#include "arch/win32/XArchWindows.h"
#include "inputleap/Screen.h"
#include "platform/MSWindowsScreen.h"
#include "platform/MSWindowsDebugOutputter.h"
#include "platform/MSWindowsWatchdog.h"
#include "platform/MSWindowsEventQueueBuffer.h"
#include "platform/MSWindowsUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>
#include <iostream>
#include <sstream>

namespace inputleap {

DaemonApp* DaemonApp::s_instance = nullptr;

int
mainLoopStatic()
{
    DaemonApp::s_instance->mainLoop(true);
    return kExitSuccess;
}

int
mainLoopStatic(int, const char**)
{
    return ArchMiscWindows::runDaemon(mainLoopStatic);
}

DaemonApp::DaemonApp() :
    m_ipcServer(nullptr),
    m_ipcLogOutputter(nullptr),
    m_watchdog(nullptr),
    m_events(nullptr),
    m_fileLogOutputter(nullptr)
{
    s_instance = this;
}

DaemonApp::~DaemonApp()
{
}

int
DaemonApp::run(int argc, char** argv)
{
    // win32 instance needed for threading, etc.
    ArchMiscWindows::setInstanceWin32(GetModuleHandle(nullptr));

    Arch arch;
    arch.init();

    Log log;
    EventQueue events;
    m_events = &events;

    bool uninstall = false;
    try
    {
        // sends debug messages to visual studio console window.
        log.insert(new MSWindowsDebugOutputter());

        // default log level to system setting.
        std::string logLevel = arch.setting("LogLevel");
        if (logLevel != "")
            log.setFilter(logLevel.c_str());

        bool foreground = false;

        for (int i = 1; i < argc; ++i) {
            std::string arg(argv[i]);

            if (arg == "/f" || arg == "-f") {
                foreground = true;
            }
            else if (arg == "/install") {
                uninstall = true;
                arch.installDaemon();
                return kExitSuccess;
            }
            else if (arg == "/uninstall") {
                arch.uninstallDaemon();
                return kExitSuccess;
            }
            else {
                std::stringstream ss;
                ss << "Unrecognized argument: " << arg;
                foregroundError(ss.str().c_str());
                return kExitArgs;
            }
        }

        if (foreground) {
            // add a console to catch Ctrl+C and run process in foreground
            // instead of daemonizing. useful for debugging.
            if (IsDebuggerPresent())
                AllocConsole();
            mainLoop(false);
        }
        else {
            arch.daemonize("Leapdesk", mainLoopStatic);
        }

        return kExitSuccess;
    }
    catch (std::runtime_error& e) {
        std::string message = e.what();
        if (uninstall && (message.find("The service has not been started") != std::string::npos)) {
            // TODO: if we're keeping this use error code instead (what is it?!).
            // HACK: this message happens intermittently, not sure where from but
            // it's quite misleading for the user. they thing something has gone
            // horribly wrong, but it's just the service manager reporting a false
            // positive (the service has actually shut down in most cases).
        }
        else {
            foregroundError(message.c_str());
        }
        return kExitFailed;
    }
    catch (std::exception& e) {
        foregroundError(e.what());
        return kExitFailed;
    }
    catch (...) {
        foregroundError("Unrecognized error.");
        return kExitFailed;
    }
}

void
DaemonApp::mainLoop(bool daemonized)
{
    try
    {
        DAEMON_RUNNING(true);

        if (daemonized) {
            m_fileLogOutputter = new FileLogOutputter(logFilename().c_str());
            CLOG->insert(m_fileLogOutputter);
        }

        // create socket multiplexer.  this must happen after daemonization
        // on unix because threads evaporate across a fork().
        SocketMultiplexer multiplexer;

        // uses event queue, must be created here.
        m_ipcServer = new IpcServer(m_events, &multiplexer);

        // send logging to gui via ipc, log system adopts outputter.
        m_ipcLogOutputter = new IpcLogOutputter(*m_ipcServer, kIpcClientGui, true);
        CLOG->insert(m_ipcLogOutputter);

        m_watchdog = new MSWindowsWatchdog(daemonized, false, *m_ipcServer, *m_ipcLogOutputter);
        m_watchdog->setFileLogOutputter(m_fileLogOutputter);

        m_events->add_handler(EventType::IPC_SERVER_MESSAGE_RECEIVED, m_ipcServer,
                              [this](const auto& event) { handle_ipc_message(event); });
        m_ipcServer->listen();

        // install the platform event queue to handle service stop events.
        m_events->set_buffer(std::make_unique<MSWindowsEventQueueBuffer>(m_events));

        start_from_settings();

        m_watchdog->startAsync();

        m_events->loop();

        m_watchdog->stop();
        delete m_watchdog;

        m_events->remove_handler(EventType::IPC_SERVER_MESSAGE_RECEIVED, m_ipcServer);

        CLOG->remove(m_ipcLogOutputter);
        delete m_ipcLogOutputter;
        delete m_ipcServer;

        DAEMON_RUNNING(false);
    }
    catch (std::exception& e) {
        LOG_CRIT("An error occurred: %s", e.what());
    }
    catch (...) {
        LOG_CRIT("An unknown error occurred.\n");
    }
}

void
DaemonApp::foregroundError(const char* message)
{
    MessageBox(nullptr, message, "Leapdesk Service", MB_OK | MB_ICONERROR);
}

std::string
DaemonApp::logFilename()
{
    std::string logFilename = ARCH->setting("LogFilename");
    if (logFilename.empty())
        logFilename = (inputleap::DataDirectories::global() / LOG_FILENAME).u8string();
    MSWindowsUtil::createDirectory(logFilename, true);
    return logFilename;
}

std::string DaemonApp::program_dir()
{
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
        return "";
    return fs::path(path).parent_path().u8string();
}

void DaemonApp::start_from_settings()
{
    auto settings = ServiceSettings::read([](const std::string& name) { return ARCH->setting(name); });

    if (settings.role.empty()) {
        // Earlier versions saved whole command lines here, including ones any local program
        // sent over IPC, so one left over isn't run. The installer carries its screen name
        // and server over into the settings above.
        if (!ARCH->setting("Command").empty())
            LOG_ERR("HKLM\\SOFTWARE\\Leapdesk holds a command line from an earlier version, "
                    "which this one doesn't run; run the Leapdesk KVM installer to set it up again");
        else
            LOG_NOTE("no role is set in HKLM\\SOFTWARE\\Leapdesk, so nothing runs until one is");
        return;
    }

    std::string problem = settings.problem();
    std::string programs = program_dir();
    if (problem.empty() && programs.empty())
        problem = "this service's own folder could not be found";
    if (!problem.empty()) {
        LOG_ERR("nothing runs: %s; fix it in HKLM\\SOFTWARE\\Leapdesk", problem.c_str());
        return;
    }

    std::string command = settings.command(programs);
    LOG_NOTE("running %s", command.c_str());
    m_watchdog->setCommand(command, settings.elevate);
}

void DaemonApp::handle_ipc_message(const Event& e)
{
    const IpcMessage& m = e.get_data_as<IpcMessage>();
    switch (m.type()) {
        case kIpcCommand:
            // Any local program can connect here, and the service runs commands with its own
            // rights, so it never runs one it is sent. What it runs comes only from
            // HKLM\SOFTWARE\Leapdesk, which only administrators can change.
            LOG_WARN("ignored a command sent to the service; it runs only what "
                     "HKLM\\SOFTWARE\\Leapdesk sets up");
            break;

        case kIpcHello:
            const auto& hm = static_cast<const IpcHelloMessage&>(m);
            std::string type;
            switch (hm.clientType()) {
                case kIpcClientGui: type = "gui"; break;
                case kIpcClientNode: type = "node"; break;
                default: type = "unknown"; break;
            }

            LOG_DEBUG("ipc hello, type=%s", type.c_str());

            const char * serverstatus = m_watchdog->isProcessActive() ? "active" : "not active";

            // using CLOG_PRINT here allows the GUI to see that the server status
            // regardless of which log level is set
            LOG_PRINT("server status: %s", serverstatus);

            m_ipcLogOutputter->notifyBuffer();
            break;
    }
}

} // namespace inputleap
