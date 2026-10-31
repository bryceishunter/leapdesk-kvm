/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2008 Volker Lanz (vl@fidra.de)
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

#include <iostream>

#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "AboutDialog.h"
#include "ServerConfigDialog.h"
#include "SettingsDialog.h"
#ifdef INPUTLEAP_GUI_ZEROCONF
#include "ZeroconfService.h"
#endif
#include "DataDownloader.h"
#include "CommandProcess.h"
#include "FingerprintAcceptDialog.h"
#include "PairingDialog.h"
#include "QUtility.h"
#include "ProcessorArch.h"
#include "SslCertificate.h"
#include "base/String.h"
#include "common/DataDirectories.h"
#include "net/FingerprintDatabase.h"
#include "net/PairedComputers.h"
#include "net/SecureUtils.h"

#include <QtCore>
#include <QtGui>
#include <QtNetwork>
#include <QNetworkAccessManager>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDesktopServices>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStyle>

#include <algorithm>

#if defined(Q_OS_MAC)
#include <ApplicationServices/ApplicationServices.h>
#endif

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

#include <signal.h>

namespace {

static const QString allFilesFilter(QObject::tr("All files (*.*)"));
#if defined(Q_OS_WIN)
static const char APP_CONFIG_NAME[] = "leapdesk.sgc";
static const QString APP_CONFIG_FILTER(QObject::tr("Leapdesk KVM Configurations (*.sgc)"));
static QString bonjourBaseUrl = "http://binaries.symless.com/bonjour/";
static const char bonjourFilename32[] = "Bonjour.msi";
static const char bonjourFilename64[] = "Bonjour64.msi";
static const char bonjourTargetFilename[] = "Bonjour.msi";
#else
static const char APP_CONFIG_NAME[] = "leapdesk.conf";
static const QString APP_CONFIG_FILTER(QObject::tr("Leapdesk KVM Configurations (*.conf)"));
#endif
static const QString APP_CONFIG_OPEN_FILTER(APP_CONFIG_FILTER + ";;" + allFilesFilter);
static const QString APP_CONFIG_SAVE_FILTER(APP_CONFIG_FILTER);

const char* icon_file_for_connection_state(AppConnectionState state)
{
#if defined(Q_OS_MAC)
    switch (state) {
        default:
        case AppConnectionState::DISCONNECTED: return ":/res/icons/128x128/leapdesk-disconnected-mask.png";
        case AppConnectionState::CONNECTING:   return ":/res/icons/128x128/leapdesk-disconnected-mask.png";
        case AppConnectionState::CONNECTED:    return ":/res/icons/128x128/leapdesk-connected-mask.png";
        case AppConnectionState::TRANSFERRING: return ":/res/icons/128x128/leapdesk-transferring-mask.png";
    }
#else
    switch (state) {
        default:
        case AppConnectionState::DISCONNECTED: return ":/res/icons/128x128/leapdesk-disconnected.png";
        case AppConnectionState::CONNECTING:   return ":/res/icons/128x128/leapdesk-disconnected.png";
        case AppConnectionState::CONNECTED:    return ":/res/icons/128x128/leapdesk-connected.png";
        case AppConnectionState::TRANSFERRING: return ":/res/icons/128x128/leapdesk-transferring.png";
    }
#endif
}

const char* icon_name_for_connection_state(AppConnectionState state)
{
    switch (state) {
        default:
        case AppConnectionState::DISCONNECTED: return "leapdesk-disconnected";
        case AppConnectionState::CONNECTING: return "leapdesk-disconnected";
        case AppConnectionState::CONNECTED: return "leapdesk-connected";
        case AppConnectionState::TRANSFERRING: return "leapdesk-transferring";
    }
}

static const char* APP_LARGE_ICON = ":/res/icons/256x256/leapdesk.png";

#if defined(Q_OS_WIN)
// Starts or stops the Leapdesk service.  Returns ERROR_SUCCESS, also when it was in that
// state already, or the Windows error.
DWORD control_leapdesk_service(bool start)
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
        return GetLastError();

    DWORD error = ERROR_SUCCESS;
    SC_HANDLE service = OpenServiceW(manager, L"Leapdesk", start ? SERVICE_START : SERVICE_STOP);
    if (service == nullptr) {
        error = GetLastError();
    } else {
        SERVICE_STATUS status;
        BOOL done = start ? StartServiceW(service, 0, nullptr)
                          : ControlService(service, SERVICE_CONTROL_STOP, &status);
        if (!done)
            error = GetLastError();
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);

    if (error == ERROR_SERVICE_ALREADY_RUNNING || error == ERROR_SERVICE_NOT_ACTIVE)
        return ERROR_SUCCESS;
    return error;
}

// The Leapdesk service's state, such as SERVICE_RUNNING, or 0 if it can't be read
DWORD leapdesk_service_state()
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
        return 0;

    DWORD state = 0;
    SC_HANDLE service = OpenServiceW(manager, L"Leapdesk", SERVICE_QUERY_STATUS);
    if (service != nullptr) {
        SERVICE_STATUS status;
        if (QueryServiceStatus(service, &status))
            state = status.dwCurrentState;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return state;
}
#endif

} // namespace

MainWindow::MainWindow(QSettings& settings, AppConfig& appConfig) :
    ui_{std::make_unique<Ui::MainWindow>()},
    m_Settings(settings),
    m_AppConfig(&appConfig),
    cmd_app_process_(nullptr),
    m_ServerConfig(&m_Settings, m_AppConfig->screenName(), this),
    m_pTrayIcon(nullptr),
    m_pTrayIconMenu(nullptr),
    m_AlreadyHidden(false),
    m_pMenuBar(nullptr),
    main_menu_(nullptr),
    m_pMenuHelp(nullptr),
    m_pZeroconfService(nullptr),
    m_pDataDownloader(nullptr),
    m_DownloadMessageBox(nullptr),
    m_pCancelButton(nullptr),
    m_SuppressAutoConfigWarning(false),
    m_BonjourInstall(nullptr),
    m_SuppressEmptyServerWarning(false),
    m_ExpectedRunningState(kStopped),
    m_pSslCertificate(nullptr),
    m_pLogWindow(new LogWindow(nullptr))
{
    // explicitly unset DeleteOnClose so the window can be show and hidden
    // repeatedly until Leapdesk KVM is finished
    setAttribute(Qt::WA_DeleteOnClose, false);
    // mark the windows as sort of "dialog" window so that tiling window
    // managers will float it by default (X11)
    setAttribute(Qt::WA_X11NetWmWindowTypeDialog, true);

    ui_->setupUi(this);
    setWindowIcon(QIcon(APP_LARGE_ICON));
    createMenuBar();
    adaptToProcessMode();
    loadSettings();
    initConnections();

    ui_->m_pLabelScreenName->setText(getScreenName());
    ui_->m_pLabelIpAddresses->setText(getIPAddresses());

#if defined(Q_OS_WIN)
    // ipc must always be enabled, so that we can disable command when switching to desktop mode.
    connect(&m_IpcClient, &IpcClient::readLogLine, this, &MainWindow::appendLogRaw);
    connect(&m_IpcClient, &IpcClient::errorMessage, this, &MainWindow::appendLogError);
    connect(&m_IpcClient, &IpcClient::infoMessage, this, &MainWindow::appendLogInfo);
    m_IpcClient.connectToHost();
#endif

    // change default size based on os
#if defined(Q_OS_MAC)
    resize(720, 550);
    setMinimumSize(720, 0);
#elif defined(Q_OS_LINUX)
    resize(700, 530);
    setMinimumSize(700, 0);
#endif

#ifdef INPUTLEAP_GUI_ZEROCONF
    m_SuppressAutoConfigWarning = true;
    ui_->m_pCheckBoxAutoConfig->setChecked(appConfig.autoConfig());
    m_SuppressAutoConfigWarning = false;
#else
    // built without Zeroconf: a stale saved setting must not disable the hostname field
    appConfig.setAutoConfig(false);
    ui_->m_pCheckBoxAutoConfig->hide();
#endif

    ui_->m_pComboServerList->hide();
    ui_->m_pLabelPadlock->hide();
    ui_->m_pLabelPadlock->setPixmap(QPixmap(":/res/icons/64x64/padlock.png").scaledToHeight(fontMetrics().height() * 1.5, Qt::SmoothTransformation));
    ui_->frame_fingerprint_details->hide();

    updateSSLFingerprint();

    // the running server or client changes the list when the other computer unpairs this one
    connect(&m_PairedWatcher, &QFileSystemWatcher::directoryChanged, this, &MainWindow::updatePairedComputers);
    updatePairedComputers();

    connect(ui_->toolbutton_show_fingerprint, &QToolButton::clicked, this, [this](bool checked)
    {
        (void) checked;

        m_fingerprint_expanded = !m_fingerprint_expanded;
        if (m_fingerprint_expanded) {
            ui_->frame_fingerprint_details->show();
            ui_->toolbutton_show_fingerprint->setArrowType(Qt::ArrowType::UpArrow);
        } else {
            ui_->frame_fingerprint_details->hide();
            ui_->toolbutton_show_fingerprint->setArrowType(Qt::ArrowType::DownArrow);
        }
    });

    // resize window to smallest reasonable size
    resize(0, 0);
}

MainWindow::~MainWindow()
{
    if (appConfig().processMode() == Desktop) {
        m_ExpectedRunningState = kStopped;
        stopDesktop();
    }

    saveSettings();

#ifdef INPUTLEAP_GUI_ZEROCONF
    delete m_pZeroconfService;
#endif
    delete m_DownloadMessageBox;
    delete m_BonjourInstall;
    delete m_pSslCertificate;

    // LogWindow is created as a sibling of the MainWindow rather than a child
    // so that the main window can be hidden without hiding the log. because of
    // this it does not get properly cleaned up by the QObject system. also by
    // the time this destructor is called the event loop will no longer be able
    // to clean up the LogWindow so ->deleteLater() will not work
    delete m_pLogWindow;
}

void MainWindow::open()
{
    createTrayIcon();

    if (appConfig().getAutoHide()) {
        hide();
    } else {
        showNormal();
    }

#ifdef INPUTLEAP_GUI_ZEROCONF
    if (!appConfig().autoConfigPrompted()) {
        promptAutoConfig();
    }
#endif

    if (serviceMode()) {
        // Windows starts the service; opening this window shouldn't undo a Stop
#if defined(Q_OS_WIN)
        if (!m_ServiceSettings.role.empty() && leapdesk_service_state() == SERVICE_RUNNING) {
            m_ExpectedRunningState = kStarted;
            set_connection_state(AppConnectionState::CONNECTING);
        }
#endif
        refreshTailscale(false);
        return;
    }

    // only start if user has previously started. this stops the gui from
    // auto hiding before the user has configured Leapdesk KVM (which of course
    // confuses first time users, who think Leapdesk KVM has crashed).
    if (appConfig().startedBefore() && appConfig().getAutoStart()) {
        m_SuppressEmptyServerWarning = true;
        start_cmd_app();
        m_SuppressEmptyServerWarning = false;
    }
}

void MainWindow::setStatus(const QString &status)
{
    ui_->m_pStatusLabel->setText(status);
}

void MainWindow::createTrayIcon()
{
    m_pTrayIconMenu = new QMenu(this);

    m_pTrayIconMenu->addAction(ui_->m_pActionStartCmdApp);
    m_pTrayIconMenu->addAction(ui_->m_pActionStopCmdApp);
    m_pTrayIconMenu->addAction(ui_->m_pActionShowLog);
    m_pTrayIconMenu->addAction(ui_->m_pActionReload);
    m_pTrayIconMenu->addSeparator();

    m_pTrayIconMenu->addAction(ui_->m_pActionMinimize);
    m_pTrayIconMenu->addAction(ui_->m_pActionRestore);
    m_pTrayIconMenu->addSeparator();
    m_pTrayIconMenu->addAction(ui_->m_pActionQuit);

    m_pTrayIcon = new QSystemTrayIcon(this);
    m_pTrayIcon->setContextMenu(m_pTrayIconMenu);
    m_pTrayIcon->setToolTip("Leapdesk KVM");

    connect(m_pTrayIcon, &QSystemTrayIcon::activated, this, &MainWindow::trayActivated);

    set_icon(AppConnectionState::DISCONNECTED);

    m_pTrayIcon->show();
}

void MainWindow::retranslateMenuBar()
{
#ifndef Q_OS_DARWIN
    main_menu_->setTitle(tr("&Leapdesk KVM"));
    m_pMenuHelp->setTitle(tr("&Help"));
#else
    m_pMenuHelp->setTitle(tr("&File"));
    main_menu_->setTitle(tr("&Window"));
#endif

}

void MainWindow::createMenuBar()
{
    m_pMenuBar = new QMenuBar(this);
    main_menu_ = new QMenu("", m_pMenuBar);
    m_pMenuHelp = new QMenu("", m_pMenuBar);
    retranslateMenuBar();

#ifndef Q_OS_DARWIN
    m_pMenuBar->addAction(main_menu_->menuAction());
    m_pMenuBar->addAction(m_pMenuHelp->menuAction());
#else
    m_pMenuBar->addAction(m_pMenuHelp->menuAction());
    m_pMenuBar->addAction(main_menu_->menuAction());
#endif

    main_menu_->addAction(ui_->m_pActionShowLog);
    main_menu_->addAction(ui_->m_pActionSettings);
    main_menu_->addAction(ui_->m_pActionMinimize);
    main_menu_->addSeparator();

#ifndef Q_OS_DARWIN
    main_menu_->addAction(ui_->m_pActionSave);
#endif
    main_menu_->addSeparator();
    main_menu_->addAction(ui_->m_pActionQuit);
    m_pMenuHelp->addAction(ui_->m_pActionAbout);

#ifdef Q_OS_DARWIN
    m_pMenuHelp->addAction(ui_->m_pActionSave);
#endif

    setMenuBar(m_pMenuBar);
}

void MainWindow::loadSettings()
{
    // the next two must come BEFORE loading groupServerChecked and groupClientChecked or
    // disabling and/or enabling the right widgets won't automatically work
    ui_->m_pGroupServer->setChecked(settings().value("groupServerChecked", false).toBool());
    ui_->m_pLineEditConfigFile->setText(settings().value("configFile",
                                                    QDir::homePath() + "/" + APP_CONFIG_NAME).toString());
    ui_->m_pGroupClient->setChecked(settings().value("groupClientChecked", true).toBool());
    ui_->m_pLineEditHostname->setText(settings().value("serverHostname").toString());

    if (serviceMode())
        loadServiceSettings();

    loadServerConfig();
}

void MainWindow::adaptToProcessMode()
{
    const bool service = serviceMode();

    // the service's settings are shown and applied here, and it reads one configuration file
    ui_->m_pWidgetScreenName->setVisible(service);
    ui_->m_pFrameApply->hide();
    ui_->m_pLabelServerPathTitle->hide();
    ui_->m_pLabelServerPath->hide();
    ui_->m_pLabelConfigurationFile->setVisible(!service);
    ui_->m_pLineEditConfigFile->setVisible(!service);
    ui_->m_pButtonBrowseConfigFile->setVisible(!service);
    ui_->label_5->setVisible(!service);
    ui_->m_pLabelScreenName->setVisible(!service);
    ui_->m_pButtonReload->setVisible(!service);
    ui_->m_pActionReload->setVisible(!service);

    bool has_tailscale = false;
#if defined(Q_OS_WIN)
    has_tailscale = QFile::exists(qEnvironmentVariable("ProgramFiles") + "/Tailscale/tailscale.exe");
#endif
    ui_->m_pButtonTailscale->setVisible(service && has_tailscale);

    if (service) {
        ui_->m_pLabelServerName->setText(tr("Server &address:"));
        ui_->m_pButtonApply->setIcon(style()->standardIcon(QStyle::SP_VistaShield));
        // the service always encrypts
        appConfig().setCryptoEnabled(true);

        connect(ui_->m_pLineEditScreenName, &QLineEdit::textEdited, this, &MainWindow::updateApplyBar,
                Qt::UniqueConnection);
        connect(ui_->m_pLineEditHostname, &QLineEdit::textEdited, this, &MainWindow::updateApplyBar,
                Qt::UniqueConnection);
        connect(ui_->m_pLineEditHostname, &QLineEdit::textEdited, this, &MainWindow::updateServerPath,
                Qt::UniqueConnection);
    }
}

void MainWindow::loadServiceSettings()
{
#if defined(Q_OS_WIN)
    QSettings service(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Leapdesk"), QSettings::NativeFormat);
    m_ServiceSettings = inputleap::ServiceSettings::read([&](const std::string& name) {
        return service.value(QString::fromStdString(name)).toString().toStdString();
    });
#endif
    auto& applied = m_ServiceSettings;
    if (applied.screen_name.empty())
        applied.screen_name = QHostInfo::localHostName().toStdString();
    if (applied.profile_dir.empty()) {
        applied.profile_dir = QDir::toNativeSeparators(QString::fromStdString(
            inputleap::DataDirectories::profile().u8string())).toStdString();
    } else {
        // certificates and the computers paired with are where the service looks for them
        inputleap::DataDirectories::profile(inputleap::fs::u8path(applied.profile_dir));
    }

    // neither is checked until a role is chosen
    {
        QSignalBlocker block_server(ui_->m_pGroupServer);
        QSignalBlocker block_client(ui_->m_pGroupClient);
        ui_->m_pGroupServer->setChecked(applied.role == "Server");
        ui_->m_pGroupClient->setChecked(applied.role == "Client");
    }

    ui_->m_pLineEditScreenName->setText(QString::fromStdString(applied.screen_name));
    ui_->m_pLineEditHostname->setText(applied.server_address.empty()
        ? settings().value("serverHostname").toString()
        : QString::fromStdString(applied.server_address));
    ui_->m_pLineEditConfigFile->setText(QDir::fromNativeSeparators(
        QString::fromStdString(applied.profile_dir)) + "/" + APP_CONFIG_NAME);
    m_ServerConfig.setServerName(QString::fromStdString(applied.screen_name));
    appConfig().setLogLevelName(QString::fromStdString(applied.log_level));

    ui_->m_pButtonToggleStart->setEnabled(!applied.role.empty());
    ui_->m_pFrameApply->setEnabled(true);
    if (applied.role.empty())
        setStatus(tr("Choose Server or Client, then press Apply."));

    updateApplyBar();
    updateServerPath();
    updatePairedComputers();
}

inputleap::ServiceSettings MainWindow::draftServiceSettings() const
{
    inputleap::ServiceSettings draft = m_ServiceSettings;
    draft.role = ui_->m_pGroupServer->isChecked() ? "Server"
               : ui_->m_pGroupClient->isChecked() ? "Client" : "";
    draft.screen_name = ui_->m_pLineEditScreenName->text().trimmed().toStdString();
    draft.server_address = ui_->m_pLineEditHostname->text().trimmed().toStdString();
    draft.log_level = m_AppConfig->logLevelText().toStdString();
    return draft;
}

void MainWindow::updateApplyBar()
{
    if (!serviceMode())
        return;

    const auto draft = draftServiceSettings();
    const auto& applied = m_ServiceSettings;
    const bool renamed = draft.screen_name != applied.screen_name;
    const bool changed = draft.role != applied.role || renamed || draft.log_level != applied.log_level ||
                         (!draft.is_server() && draft.server_address != applied.server_address);

    // pairing tells the other computer the name and address the service uses
    const QString apply_first = changed ? tr("Apply your changes first.") : QString();
    ui_->m_pButtonPairClient->setEnabled(!changed && applied.is_server());
    ui_->m_pButtonPairClient->setToolTip(apply_first);
    ui_->m_pButtonPairServer->setEnabled(!changed && applied.role == "Client");
    ui_->m_pButtonPairServer->setToolTip(apply_first);

    if (draft.role.empty() || !changed) {
        ui_->m_pFrameApply->hide();
        return;
    }

    const std::string problem = draft.problem();
    QString text;
    if (!problem.empty()) {
        text = tr("Can't apply this yet: %1.").arg(QString::fromStdString(problem));
    } else {
        text = tr("Applying restarts the Leapdesk service. Windows asks for administrator approval.");
        if (renamed && !draft.is_server())
            text += " " + tr("The server's layout has to use the new name too.");
    }
    ui_->m_pLabelApply->setText(text);
    ui_->m_pButtonApply->setEnabled(problem.empty());
    ui_->m_pFrameApply->show();
}

void MainWindow::on_m_pButtonUndo_clicked()
{
    loadServiceSettings();
    loadServerConfig();
}

void MainWindow::on_m_pButtonApply_clicked()
{
#if defined(Q_OS_WIN)
    const auto draft = draftServiceSettings();
    if (!draft.problem().empty())
        return;

    if (draft.is_server()) {
        // the server won't start without a configuration holding its own screen
        loadServerConfig();
        if (!m_ServerConfigError.isEmpty()) {
            QMessageBox::warning(this, tr("Configuration file has an error"),
                                 tr("%1 could not be read: %2\n\nFix it in a text editor first.")
                                     .arg(ui_->m_pLineEditConfigFile->text(), m_ServerConfigError));
            return;
        }
        m_ServerConfig.setServerName(QString::fromStdString(draft.screen_name));
        m_ServerConfig.ensureServerScreen(QString::fromStdString(m_ServiceSettings.screen_name));
        if (!saveServerConfig())
            return;
    }

    const std::wstring program = QDir::toNativeSeparators(appPath("leapdesk-daemon.exe")).toStdWString();
    const std::wstring parameters = QString::fromStdString(
        "/configure " + draft.configure_arguments()).toStdWString();

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = reinterpret_cast<HWND>(winId());
    info.lpVerb = L"runas";
    info.lpFile = program.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        DWORD error = GetLastError();
        ui_->m_pLabelApply->setText(error == ERROR_CANCELLED
            ? tr("Nothing changed, since administrator approval was declined.")
            : tr("Couldn't run the Leapdesk service program: %1").arg(qt_error_string(static_cast<int>(error))));
        return;
    }

    // it may wait some seconds for the running server or client to stop
    ui_->m_pFrameApply->setEnabled(false);
    ui_->m_pLabelApply->setText(tr("Restarting the Leapdesk service..."));

    auto* timer = new QTimer(this);
    HANDLE process = info.hProcess;
    connect(timer, &QTimer::timeout, this, [this, timer, process]() {
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            return;
        timer->stop();
        timer->deleteLater();

        DWORD exit_code = 1;
        GetExitCodeProcess(process, &exit_code);
        CloseHandle(process);

        if (exit_code != 0) {
            // it has told the user why
            appendLogError(QString("the service settings weren't applied, exit code %1").arg(exit_code));
            ui_->m_pFrameApply->setEnabled(true);
            updateApplyBar();
            return;
        }

        loadServiceSettings();
        m_pLogWindow->startNewInstance();
        m_ExpectedRunningState = kStarted;
        set_connection_state(AppConnectionState::CONNECTING);
        refreshTailscale(false);
    });
    timer->start(250);
#endif
}

void MainWindow::on_m_pButtonTailscale_clicked()
{
    refreshTailscale(true);
}

void MainWindow::refreshTailscale(bool showDevices)
{
#if defined(Q_OS_WIN)
    m_ShowTailscaleDevices = m_ShowTailscaleDevices || showDevices;
    if (m_TailscaleProcess != nullptr)
        return;

    const QString program = qEnvironmentVariable("ProgramFiles") + "/Tailscale/tailscale.exe";
    if (!QFile::exists(program))
        return;

    m_TailscaleProcess = new QProcess(this);
    auto finished = [this]() {
        if (m_TailscaleProcess == nullptr)
            return;
        m_TailscalePeers = parse_tailscale_status(m_TailscaleProcess->readAllStandardOutput());
        m_TailscaleProcess->deleteLater();
        m_TailscaleProcess = nullptr;

        updateServerPath();
        updatePairedComputers();
        if (m_ShowTailscaleDevices) {
            m_ShowTailscaleDevices = false;
            showTailscaleDevices();
        }
    };
    connect(m_TailscaleProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, finished);
    connect(m_TailscaleProcess, &QProcess::errorOccurred, this, [finished](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finished();
    });
    m_TailscaleProcess->start(program, {"status", "--json"});
#else
    (void) showDevices;
#endif
}

void MainWindow::showTailscaleDevices()
{
    if (m_TailscalePeers.isEmpty()) {
        QMessageBox::information(this, tr("Tailscale"),
                                 tr("Tailscale listed no devices. Check that it's running and signed in."));
        return;
    }

    QMenu menu(this);
    for (const TailscalePeer& peer : m_TailscalePeers) {
        QString state = peer.path == TailscalePeer::Path::Offline ? tr("offline") : tr("online");
        QAction* action = menu.addAction(QString("%1  %2  (%3)").arg(peer.name, peer.address, state));
        action->setData(peer.address);
    }

    QAction* chosen = menu.exec(ui_->m_pButtonTailscale->mapToGlobal(ui_->m_pButtonTailscale->rect().bottomLeft()));
    if (chosen != nullptr) {
        ui_->m_pLineEditHostname->setText(chosen->data().toString());
        updateApplyBar();
        updateServerPath();
    }
}

void MainWindow::updateServerPath()
{
    const TailscalePeer* peer = nullptr;
    if (serviceMode() && ui_->m_pGroupClient->isChecked())
        peer = find_tailscale_peer(m_TailscalePeers, ui_->m_pLineEditHostname->text());

    ui_->m_pLabelServerPathTitle->setVisible(peer != nullptr);
    ui_->m_pLabelServerPath->setVisible(peer != nullptr);
    if (peer == nullptr)
        return;

    switch (peer->path) {
    case TailscalePeer::Path::Direct:
        ui_->m_pLabelServerPath->setText(tr("Direct, through %1").arg(peer->direct_address));
        break;
    case TailscalePeer::Path::Relayed:
        ui_->m_pLabelServerPath->setText(tr("Relayed through Tailscale (%1), so the pointer may lag").arg(peer->relay));
        break;
    case TailscalePeer::Path::Idle:
        ui_->m_pLabelServerPath->setText(tr("Online on Tailscale, nothing being sent yet"));
        break;
    case TailscalePeer::Path::Offline:
        ui_->m_pLabelServerPath->setText(tr("Offline on Tailscale"));
        break;
    }
}

void MainWindow::loadServerConfig()
{
    QString fileName = ui_->m_pLineEditConfigFile->text();
    m_ServerConfigError.clear();

    if (QFile::exists(fileName)) {
        if (!m_ServerConfig.load(fileName, &m_ServerConfigError))
            appendLogError(tr("server configuration %1 has an error, %2")
                               .arg(fileName, m_ServerConfigError));
        return;
    }

    // older versions kept the layout in the settings; move it into the file
    if (m_ServerConfig.loadLegacySettings()) {
        if (saveServerConfig())
            appendLogInfo(tr("moved the server configuration into %1").arg(fileName));
        return;
    }

    m_ServerConfig.reset();
}

bool MainWindow::saveServerConfig()
{
    QString fileName = ui_->m_pLineEditConfigFile->text();
    QString error;
    if (!m_ServerConfig.save(fileName, &error)) {
        QMessageBox::warning(this, tr("Could not save the configuration"),
                             tr("The server configuration could not be written to %1:\n\n%2")
                                 .arg(fileName, error));
        return false;
    }
    m_ServerConfigError.clear();
    return true;
}

void MainWindow::initConnections()
{
    connect(ui_->m_pActionMinimize, &QAction::triggered, this, &MainWindow::hide);
    connect(ui_->m_pComboServerList, &QComboBox::currentTextChanged, this, &MainWindow::comboServerList_currentIndexChanged);
    connect(ui_->m_pActionRestore, &QAction::triggered, this, &MainWindow::showNormal);
    connect(ui_->m_pActionStartCmdApp, &QAction::triggered, this, &MainWindow::start_cmd_app);
    connect(ui_->m_pActionStopCmdApp, &QAction::triggered, this, &MainWindow::stop_cmd_app);
    connect(ui_->m_pActionShowLog, &QAction::triggered, this, &MainWindow::showLogWindow);
    connect(ui_->m_pActionReload, &QAction::triggered, this, &MainWindow::restart_cmd_app);
    connect(ui_->m_pActionQuit, &QAction::triggered, qApp, &QCoreApplication::quit);
}

void MainWindow::saveSettings()
{
    // program settings
    settings().setValue("groupServerChecked", ui_->m_pGroupServer->isChecked());
    settings().setValue("configFile", ui_->m_pLineEditConfigFile->text());
    settings().setValue("groupClientChecked", ui_->m_pGroupClient->isChecked());
    settings().setValue("serverHostname", ui_->m_pLineEditHostname->text());

    settings().sync();
}

void MainWindow::set_icon(AppConnectionState state)
{
    if (m_pTrayIcon) {
        QIcon icon = QIcon::fromTheme(icon_name_for_connection_state(state),
                                      QIcon(icon_file_for_connection_state(state)));
#if defined(Q_OS_MAC)
        icon.setIsMask(true);
#endif
        m_pTrayIcon->setIcon(icon);
    }
}

void MainWindow::trayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::DoubleClick)
    {
        if (isVisible())
        {
            hide();
        }
        else
        {
            showNormal();
            activateWindow();
        }
    }
}

void MainWindow::logOutput()
{
    if (cmd_app_process_)
    {
        QString text(cmd_app_process_->readAllStandardOutput());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const auto results = text.split(QRegularExpression("\r|\n|\r\n"));
#else
        const auto results = text.split(QRegExp("\r|\n|\r\n"));
#endif
        for (const auto& line : results) {
            if (!line.isEmpty())
            {
                appendLogRaw(line);
            }
        }
    }
}

void MainWindow::logError()
{
    if (cmd_app_process_)
    {
        appendLogRaw(cmd_app_process_->readAllStandardError());
    }
}

void MainWindow::appendLogInfo(const QString& text)
{
    if (appConfig().logLevel() >= 3) {
        m_pLogWindow->appendInfo(text);
    }
}

void MainWindow::appendLogDebug(const QString& text) {
    if (appConfig().logLevel() >= 4) {
        m_pLogWindow->appendDebug(text);
    }
}

void MainWindow::appendLogError(const QString& text)
{
    m_pLogWindow->appendError(text);
}

void MainWindow::appendLogRaw(const QString& text)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const auto lines = text.split(QRegularExpression("\r|\n|\r\n"));
#else
    const auto lines = text.split(QRegExp("\r|\n|\r\n"));
#endif
    for (const auto& line : lines) {
        if (!line.isEmpty()) {
            m_pLogWindow->appendRaw(line);
            updateFromLogLine(line);
        }
    }
}

void MainWindow::updateFromLogLine(const QString &line)
{
    // TODO: this code makes Andrew cry
    checkConnected(line);
    checkFingerprint(line);
}

void MainWindow::checkConnected(const QString& line)
{
    // TODO: implement ipc connection state messages to replace this hack.
    if (serviceMode() && m_ServiceSettings.role.empty())
        return;

    if (line.contains("started server") ||
        line.contains("connected to server") ||
        line.contains("server status: active"))
    {
        set_connection_state(AppConnectionState::CONNECTED);

        if (!appConfig().startedBefore() && isVisible()) {
                QMessageBox::information(
                    this, "Leapdesk KVM",
                    tr("Leapdesk KVM is now connected. You can close the "
                    "config window and Leapdesk KVM will remain connected in "
                    "the background."));

            appConfig().setStartedBefore(true);
            appConfig().saveSettings();
        }
    }
}

void MainWindow::checkFingerprint(const QString& line)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QRegularExpression fingerprintRegex("peer fingerprint \\(SHA1\\): ([A-F0-9:]+) \\(SHA256\\): ([A-F0-9:]+)$");
    QRegularExpressionMatch match = fingerprintRegex.match(line);
    if (!match.hasMatch()) {
        return;
    }

    auto match1 = match.captured(1).toStdString();
    auto match2 = match.captured(2).toStdString();
#else
    QRegExp fingerprintRegex(".*peer fingerprint \\(SHA1\\): ([A-F0-9:]+) \\(SHA256\\): ([A-F0-9:]+)");
    if (!fingerprintRegex.exactMatch(line)) {
        return;
    }

    auto match1 = fingerprintRegex.cap(1).toStdString();
    auto match2 = fingerprintRegex.cap(2).toStdString();
#endif

    inputleap::FingerprintData fingerprint_sha1 = {
        inputleap::fingerprint_type_to_string(inputleap::FingerprintType::SHA1),
        inputleap::string::from_hex(match1)
    };

    inputleap::FingerprintData fingerprint_sha256 = {
        inputleap::fingerprint_type_to_string(inputleap::FingerprintType::SHA256),
        inputleap::string::from_hex(match2)
    };

    bool is_client = app_role() == AppRole::Client;

    auto db_path = is_client
            ? inputleap::DataDirectories::trusted_servers_ssl_fingerprints_path()
            : inputleap::DataDirectories::trusted_clients_ssl_fingerprints_path();

    auto db_dir = db_path.parent_path();
    if (!inputleap::fs::exists(db_dir)) {
        inputleap::fs::create_directories(db_dir);
    }

    // We compare only SHA256 fingerprints, but show both SHA1 and SHA256 so that the users can
    // still verify fingerprints on old Input Leap servers. This way the only time when we are
    // exposed to SHA1 vulnerabilities is when the user is reconnecting again.
    inputleap::FingerprintDatabase db;
    db.read(db_path);
    if (db.is_trusted(fingerprint_sha256)) {
        return;
    }

    if (serviceMode()) {
        // computers pair by comparing a code instead, and the service keeps trying meanwhile
        setStatus(is_client
            ? tr("This computer isn't paired with the server yet. Press Pair with Server here, "
                 "and Pair a Computer on the server.")
            : tr("A computer that isn't paired tried to connect. To let it in, press Pair a Computer "
                 "here, and Pair with Server on that computer."));
        return;
    }

    static bool messageBoxAlreadyShown = false;

    if (!messageBoxAlreadyShown) {
        if (is_client) {
            stop_cmd_app();
        }

        messageBoxAlreadyShown = true;
        FingerprintAcceptDialog dialog{this, app_role(), fingerprint_sha1, fingerprint_sha256};
        if (dialog.exec() == QDialog::Accepted) {
            // restart core process after trusting fingerprint.
            db.add_trusted(fingerprint_sha256);
            db.write(db_path);
            if (is_client) {
                start_cmd_app();
            }
        }

        messageBoxAlreadyShown = false;
    }
}

void MainWindow::restart_cmd_app()
{
    stop_cmd_app();
    start_cmd_app();
}

void MainWindow::proofreadInfo()
{
    AppConnectionState old = connection_state_;
    connection_state_ = AppConnectionState::DISCONNECTED;
    set_connection_state(old);
}

void MainWindow::start_cmd_app()
{
    if (serviceMode() && m_ServiceSettings.role.empty())
        return;

    appendLogDebug("starting process");
    m_ExpectedRunningState = kStarted;
    set_connection_state(AppConnectionState::CONNECTING);

    if (appConfig().processMode() == Service) {
        // the service runs what HKLM\SOFTWARE\Leapdesk sets up, which only an administrator
        // can change; this window can only start and stop it
        m_pLogWindow->startNewInstance();
        controlService(true);
        return;
    }

    QString app;
    QStringList args;

    args << "-f" << "--no-tray" << "--debug" << appConfig().logLevelText();
    args << "--name" << getScreenName();

    cmd_app_process_ = new QProcess(this);

#ifndef Q_OS_LINUX

    if (m_ServerConfig.enableDragAndDrop()) {
        args << "--enable-drag-drop";
    }

#endif

    if (!m_AppConfig->getCryptoEnabled()) {
        args << "--disable-crypto";
    }

#if defined(Q_OS_WIN)
    // on windows, the profile directory changes depending on the user that
    // launched the process (e.g. when launched with elevation). setting the
    // profile dir on launch ensures it uses the same profile dir is used
    // no matter how its relaunched.
    args << "--profile-dir" << QString::fromStdString("\"" + inputleap::DataDirectories::profile().u8string() + "\"");
#endif

    if ((app_role() == AppRole::Client && !clientArgs(args, app))
        || (app_role() == AppRole::Server && !serverArgs(args, app)))
    {
        stop_cmd_app();
        return;
    }

    connect(cmd_app_process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MainWindow::cmd_app_finished);
    connect(cmd_app_process_, &QProcess::readyReadStandardOutput, this, &MainWindow::logOutput);
    connect(cmd_app_process_, &QProcess::readyReadStandardError, this, &MainWindow::logError);

    m_pLogWindow->startNewInstance();

    appendLogInfo("starting " + QString(app_role() == AppRole::Server ? "server" : "client"));

    qDebug() << args;

    appendLogDebug(QString("command: %1 %2").arg(app, args.join(" ")));

    appendLogInfo("config file: " + configFilename());
    appendLogInfo("log level: " + appConfig().logLevelText());

    if (appConfig().logToFile())
        appendLogInfo("log file: " + appConfig().logFilename());

    cmd_app_process_->start(app, args);
    if (!cmd_app_process_->waitForStarted())
    {
        show();
        QMessageBox::warning(this, tr("Program can not be started"), QString(tr("The executable<br><br>%1<br><br>could not be successfully started, although it does exist. Please check if you have sufficient permissions to run this program.").arg(app)));
        return;
    }
}

void MainWindow::controlService(bool start, int tries)
{
#if defined(Q_OS_WIN)
    // a stop takes a moment, and the service can't start again until it has finished,
    // which matters when Reload stops and starts it
    if (start && tries > 0 && leapdesk_service_state() == SERVICE_STOP_PENDING) {
        QTimer::singleShot(500, this, [this, tries]() {
            if (m_ExpectedRunningState == kStarted)
                controlService(true, tries - 1);
        });
        return;
    }

    const QString action = start ? QStringLiteral("start") : QStringLiteral("stop");
    DWORD error = control_leapdesk_service(start);

    if (error == ERROR_ACCESS_DENIED) {
        // installed before the service let signed-in users start and stop it, so an
        // administrator has to, once
        appendLogInfo(QString("asking for administrator approval to %1 the Leapdesk service").arg(action));
        std::wstring parameters = (action + QStringLiteral(" Leapdesk")).toStdWString();
        SHELLEXECUTEINFOW info = {};
        info.cbSize = sizeof(info);
        info.hwnd = reinterpret_cast<HWND>(winId());
        info.lpVerb = L"runas";
        info.lpFile = L"sc.exe";
        info.lpParameters = parameters.c_str();
        info.nShow = SW_HIDE;
        error = ShellExecuteExW(&info) ? ERROR_SUCCESS : GetLastError();
    }

    if (error == ERROR_SUCCESS)
        return;

    QString reason = error == ERROR_SERVICE_DOES_NOT_EXIST
        ? tr("The Leapdesk service isn't installed. Run the Leapdesk KVM installer.")
        : error == ERROR_CANCELLED ? tr("Administrator approval was declined.")
        : qt_error_string(static_cast<int>(error));
    appendLogError(QString("couldn't %1 the Leapdesk service: %2").arg(action, reason));
    if (start) {
        m_ExpectedRunningState = kStopped;
        set_connection_state(AppConnectionState::DISCONNECTED);
    }
#else
    (void) start;
    (void) tries;
#endif
}

void MainWindow::setServerMode(bool isServerMode)
{
    ui_->m_pGroupServer->setChecked(isServerMode);
    ui_->m_pGroupClient->setChecked(!isServerMode);
}

bool MainWindow::clientArgs(QStringList& args, QString& app)
{
    app = appPath(appConfig().client_name());

    if (!QFile::exists(app))
    {
        show();
        QMessageBox::warning(this, tr("Leapdesk KVM client not found"),
                             tr("The executable for the Leapdesk KVM client does not exist."));
        return false;
    }

#if defined(Q_OS_WIN)
    // wrap in quotes so a malicious user can't start \Program.exe as admin.
    app = QString("\"%1\"").arg(app);
#endif

    if (appConfig().logToFile())
    {
        appConfig().persistLogDir();
        args << "--log" << appConfig().logFilenameCmd();
    }

    // check auto config first, if it is disabled or no server detected,
    // use line edit host name if it is not empty
    if (ui_->m_pCheckBoxAutoConfig->isChecked()) {
        if (ui_->m_pComboServerList->count() != 0) {
            QString serverIp = ui_->m_pComboServerList->currentText();
            args << "[" + serverIp + "]:" + QString::number(appConfig().port());
            return true;
        }
    } else if (ui_->m_pLineEditHostname->text().isEmpty()) {
        show();
        if (!m_SuppressEmptyServerWarning) {
            QMessageBox::warning(this, tr("Hostname is empty"),
                             tr("Please fill in a hostname for the Leapdesk KVM client to connect to."));
        }
        return false;
    }

    args << "[" + ui_->m_pLineEditHostname->text() + "]:" + QString::number(appConfig().port());

    return true;
}

QString MainWindow::configFilename()
{
    // the server reads the same file the settings window edits; a new
    // configuration gets written out the first time the server needs it
    QString filename = ui_->m_pLineEditConfigFile->text();
    if (!QFile::exists(filename) && !saveServerConfig())
        return "";
    return filename;
}

AppRole MainWindow::app_role() const
{
    // what the service runs, whatever is chosen but not applied in this window
    if (serviceMode() && !m_ServiceSettings.role.empty())
        return m_ServiceSettings.is_server() ? AppRole::Server : AppRole::Client;
    return ui_->m_pGroupClient->isChecked() ? AppRole::Client : AppRole::Server;
}

QString MainWindow::hostname() const
{
    return ui_->m_pLineEditHostname->text();
}

QString MainWindow::address()
{
    QString address = appConfig().networkInterface();
    if (!address.isEmpty())
        address = "[" + address + "]";
    return address + ":" + QString::number(appConfig().port());
}

QString MainWindow::appPath(const QString& name)
{
    return appConfig().program_dir() + name;
}

bool MainWindow::serverArgs(QStringList& args, QString& app)
{
    app = appPath(appConfig().server_name());

    if (!QFile::exists(app))
    {
        QMessageBox::warning(this, tr("Leapdesk KVM server not found"),
                             tr("The executable for the Leapdesk KVM server does not exist."));
        return false;
    }

#if defined(Q_OS_WIN)
    // wrap in quotes so a malicious user can't start \Program.exe as admin.
    app = QString("\"%1\"").arg(app);
#endif

    if (appConfig().logToFile())
    {
        appConfig().persistLogDir();

        args << "--log" << appConfig().logFilenameCmd();
    }

    if (!appConfig().getRequireClientCertificate()) {
        args << "--disable-client-cert-checking";
    }

    QString configFilename = this->configFilename();
#if defined(Q_OS_WIN)
    // wrap in quotes in case username contains spaces.
    configFilename = QString("\"%1\"").arg(configFilename);
#endif
    args << "-c" << configFilename << "--address" << address();

    return true;
}

void MainWindow::stop_cmd_app()
{
    appendLogDebug("stopping process");

    m_ExpectedRunningState = kStopped;

    if (appConfig().processMode() == Service)
    {
        stopService();
    }
    else if (appConfig().processMode() == Desktop)
    {
        stopDesktop();
    }

    set_connection_state(AppConnectionState::DISCONNECTED);

    // reset so that new connects cause auto-hide.
    m_AlreadyHidden = false;
}

void MainWindow::stopService()
{
    controlService(false);
}

void MainWindow::stopDesktop()
{
    QMutexLocker locker(&m_StopDesktopMutex);
    if (!cmd_app_process_) {
        return;
    }

    appendLogInfo("stopping Leapdesk KVM desktop process");

    if (cmd_app_process_->isOpen()) {
#if SYSAPI_UNIX
        kill(cmd_app_process_->processId(), SIGTERM);
        cmd_app_process_->waitForFinished(5000);
#endif
        cmd_app_process_->close();
    }

    delete cmd_app_process_;
    cmd_app_process_ = nullptr;
}

void MainWindow::cmd_app_finished(int exitCode, QProcess::ExitStatus)
{
    if (exitCode == 0) {
        appendLogInfo(QString("process exited normally"));
    }
    else {
        appendLogError(QString("process exited with error code: %1").arg(exitCode));
    }

    if (m_ExpectedRunningState == kStarted) {
        QTimer::singleShot(1000, this, &MainWindow::start_cmd_app);
        appendLogInfo(QString("detected process not running, auto restarting"));
    }
    else {
        set_connection_state(AppConnectionState::DISCONNECTED);
    }
}

void MainWindow::set_connection_state(AppConnectionState state)
{
    if (connection_state() == state)
        return;

    if (state == AppConnectionState::CONNECTED || state == AppConnectionState::CONNECTING)
    {
        disconnect(ui_->m_pButtonToggleStart, &QPushButton::clicked, ui_->m_pActionStartCmdApp, &QAction::trigger);
        connect(ui_->m_pButtonToggleStart, &QPushButton::clicked, ui_->m_pActionStopCmdApp, &QAction::trigger);
        ui_->m_pButtonToggleStart->setText(tr("&Stop"));
        ui_->m_pButtonReload->setEnabled(true);
    }
    else if (state == AppConnectionState::DISCONNECTED)
    {
        disconnect(ui_->m_pButtonToggleStart, &QPushButton::clicked, ui_->m_pActionStopCmdApp, &QAction::trigger);
        connect(ui_->m_pButtonToggleStart, &QPushButton::clicked, ui_->m_pActionStartCmdApp, &QAction::trigger);
        ui_->m_pButtonToggleStart->setText(tr("&Start"));
        ui_->m_pButtonReload->setEnabled(false);
    }

    bool connected = false;
    if (state == AppConnectionState::CONNECTED || state == AppConnectionState::TRANSFERRING) {
        connected = true;
    }

    ui_->m_pActionStartCmdApp->setEnabled(!connected);
    ui_->m_pActionStopCmdApp->setEnabled(connected);

    switch (state)
    {
    case AppConnectionState::CONNECTED: {
        if (m_AppConfig->getCryptoEnabled()) {
            ui_->m_pLabelPadlock->show();
        }
        else {
            ui_->m_pLabelPadlock->hide();
        }

        setStatus(tr("Leapdesk KVM is running."));

        break;
    }
    case AppConnectionState::CONNECTING:
        ui_->m_pLabelPadlock->hide();
        setStatus(tr("Leapdesk KVM is starting."));
        break;
    case AppConnectionState::DISCONNECTED:
        ui_->m_pLabelPadlock->hide();
        setStatus(tr("Leapdesk KVM is not running."));
        break;
    case AppConnectionState::TRANSFERRING:
        break;
    default:
        break;
    }

    set_icon(state);

    connection_state_ = state;
}

void MainWindow::setVisible(bool visible)
{
    QMainWindow::setVisible(visible);
    ui_->m_pActionMinimize->setEnabled(visible);
    ui_->m_pActionRestore->setEnabled(!visible);

#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070 // lion
    // dock hide only supported on lion :(
    ProcessSerialNumber psn = { 0, kCurrentProcess };
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    GetCurrentProcess(&psn);
#pragma GCC diagnostic pop
    if (visible)
        TransformProcessType(&psn, kProcessTransformToForegroundApplication);
    else
        TransformProcessType(&psn, kProcessTransformToBackgroundApplication);
#endif
}

QString MainWindow::getIPAddresses()
{
    QList<QHostAddress> addresses = QNetworkInterface::allAddresses();

    bool hinted = false;
    QString result;

    // a Tailscale address is the most useful one, since it works from anywhere on the tailnet
    auto is_tailscale = [](const QHostAddress& address) {
        return address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10);
    };
    const bool has_tailscale = std::any_of(addresses.begin(), addresses.end(), is_tailscale);

    for (int i = 0; i < addresses.size(); i++) {
        if (addresses[i].protocol() == QAbstractSocket::IPv4Protocol &&
            addresses[i] != QHostAddress(QHostAddress::LocalHost)) {

            QString address = addresses[i].toString();
            QString format = "%1, ";

            // usually 192.168.x.x is a useful ip for the user, so indicate
            // this by making it bold.
            if (!hinted && (has_tailscale ? is_tailscale(addresses[i]) : address.startsWith("192.168"))) {
                hinted = true;
                format = "<b>%1</b>, ";
            }

            result += format.arg(address);
        }
    }

    if (result == "") {
        return tr("Unknown");
    }

    // remove trailing comma.
    result.chop(2);

    return result;
}

QString MainWindow::getScreenName()
{
    if (serviceMode()) {
        return QString::fromStdString(m_ServiceSettings.screen_name);
    }

    if (appConfig().screenName() == "") {
        return QHostInfo::localHostName();
    }
    else {
        return appConfig().screenName();
    }
}

void MainWindow::changeEvent(QEvent* event)
{
    if (event != nullptr)
    {
        switch (event->type())
        {
        case QEvent::LanguageChange:
        {
            ui_->retranslateUi(this);
            retranslateMenuBar();
            if (serviceMode()) {
                ui_->m_pLabelServerName->setText(tr("Server &address:"));
                updateApplyBar();
                updateServerPath();
            }

            proofreadInfo();

            break;
        }
        case QEvent::WindowStateChange:
        {
            windowStateChanged();
            break;
        }
        default:
        {
            break;
        }
        }
    }
    // all that do not return are allowing the event to propagate
    QMainWindow::changeEvent(event);
}

bool MainWindow::event(QEvent* event)
{
    if (event->type() == QEvent::LayoutRequest) {
        setFixedSize(sizeHint());
    }
    return QMainWindow::event(event);
}

void MainWindow::updateZeroconfService()
{
#ifdef INPUTLEAP_GUI_ZEROCONF
    QMutexLocker locker(&m_UpdateZeroconfMutex);

    if (isBonjourRunning()) {
        if (!m_AppConfig->wizardShouldRun()) {
            if (m_pZeroconfService) {
                delete m_pZeroconfService;
                m_pZeroconfService = nullptr;
            }

            if (m_AppConfig->autoConfig() || app_role() == AppRole::Server) {
                m_pZeroconfService = new ZeroconfService(this);
            }
        }
    }
#endif
}

void MainWindow::serverDetected(const QString name)
{
    if (ui_->m_pComboServerList->findText(name) == -1) {
        // Note: the first added item triggers restart_cmd_app
        ui_->m_pComboServerList->addItem(name);
    }

    if (ui_->m_pComboServerList->count() > 1) {
        ui_->m_pComboServerList->show();
    }
}

void MainWindow::updateSSLFingerprint()
{
    if (m_AppConfig->getCryptoEnabled() && m_pSslCertificate == nullptr) {
        m_pSslCertificate = new SslCertificate(this);
        connect(m_pSslCertificate, &SslCertificate::info, this, &MainWindow::appendLogInfo);
        m_pSslCertificate->generateCertificate();
    }

    ui_->toolbutton_show_fingerprint->setEnabled(false);
    ui_->m_pLabelLocalFingerprint->setText("Disabled");

    if (!m_AppConfig->getCryptoEnabled()) {
        return;
    }

    auto local_path = inputleap::DataDirectories::local_ssl_fingerprints_path();
    if (!inputleap::fs::exists(local_path)) {
        return;
    }

    inputleap::FingerprintDatabase db;
    db.read(local_path);
    if (db.fingerprints().size() != 2) {
        return;
    }

    for (const auto& fingerprint : db.fingerprints()) {
        if (fingerprint.algorithm == "sha1") {
            auto fingerprint_str = inputleap::format_ssl_fingerprint(fingerprint.data);
            ui_->label_sha1_fingerprint_full->setText(QString::fromStdString(fingerprint_str));
            continue;
        }

        if (fingerprint.algorithm == "sha256") {
            auto fingerprint_str = inputleap::format_ssl_fingerprint(fingerprint.data);
            fingerprint_str.resize(40);
            fingerprint_str += " ...";

            auto fingerprint_str_cols = inputleap::format_ssl_fingerprint_columns(fingerprint.data);
            auto fingerprint_randomart = inputleap::create_fingerprint_randomart(fingerprint.data);

            ui_->m_pLabelLocalFingerprint->setText(QString::fromStdString(fingerprint_str));
            ui_->label_sha256_fingerprint_full->setText(QString::fromStdString(fingerprint_str_cols));
            ui_->label_sha256_randomart->setText(QString::fromStdString(fingerprint_randomart));
        }
    }

    ui_->toolbutton_show_fingerprint->setEnabled(true);
}

void MainWindow::on_m_pGroupClient_toggled(bool on)
{
    ui_->m_pGroupServer->setChecked(!on);
    if (on) {
        updateZeroconfService();
    }
    updateApplyBar();
    updateServerPath();
    updatePairedComputers();
}

void MainWindow::on_m_pGroupServer_toggled(bool on)
{
    ui_->m_pGroupClient->setChecked(!on);
    if (on) {
        updateZeroconfService();
    }
    updateApplyBar();
    updateServerPath();
    updatePairedComputers();
}

bool MainWindow::on_m_pButtonBrowseConfigFile_clicked()
{
    // pick an existing file to edit, or name a new one to start from the current configuration
    QString fileName = QFileDialog::getSaveFileName(
        this, tr("Choose the server configuration file"), ui_->m_pLineEditConfigFile->text(),
        APP_CONFIG_OPEN_FILTER, nullptr, QFileDialog::DontConfirmOverwrite);
    if (fileName.isEmpty())
        return false;

    QString error;
    if (QFile::exists(fileName)) {
        if (!m_ServerConfig.load(fileName, &error)) {
            QMessageBox::warning(this, tr("Configuration file has an error"),
                                 tr("%1 could not be read: %2\n\nFix it in a text editor, "
                                    "or choose another file.").arg(fileName, error));
            return false;
        }
    } else if (!m_ServerConfig.exportTo(fileName, &error) || !m_ServerConfig.load(fileName, &error)) {
        QMessageBox::warning(this, tr("Could not create the configuration"),
                             tr("%1 could not be written: %2").arg(fileName, error));
        return false;
    }

    bool changed = fileName != ui_->m_pLineEditConfigFile->text();
    ui_->m_pLineEditConfigFile->setText(fileName);
    m_ServerConfigError.clear();

    // a running server follows the file it was started with, so point it at the new one
    if (changed && m_ExpectedRunningState == kStarted && app_role() == AppRole::Server)
        restart_cmd_app();
    return true;
}

bool MainWindow::on_m_pActionSave_triggered()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save configuration as..."), QString(), APP_CONFIG_SAVE_FILTER);

    QString error;
    if (!fileName.isEmpty() && !serverConfig().exportTo(fileName, &error))
    {
        QMessageBox::warning(this, tr("Save failed"),
                             tr("Could not save configuration to file: %1").arg(error));
        return true;
    }

    return false;
}

void MainWindow::on_m_pActionAbout_triggered()
{
    AboutDialog(this, appPath(appConfig().client_name())).exec();
}

void MainWindow::on_m_pActionSettings_triggered()
{
    auto dialog = std::make_unique<SettingsDialog>(this, appConfig());
    connect(dialog.get(), &SettingsDialog::requestLanguageChange, this, &MainWindow::requestLanguageChange);
    if (dialog.get()->exec() == QDialog::Accepted) {
        updateSSLFingerprint();
        updateApplyBar();
    }
    disconnect(dialog.get(), &SettingsDialog::requestLanguageChange, this, &MainWindow::requestLanguageChange);
}

void MainWindow::autoAddScreen(const QString name)
{
    if (!m_ServerConfig.ignoreAutoConfigClient()) {
        int r = m_ServerConfig.autoAddScreen(name);
        if (r != kAutoAddScreenOk) {
            switch (r) {
            case kAutoAddScreenManualServer:
                showConfigureServer(
                    tr("Please add the server (%1) to the screens.")
                        .arg(appConfig().screenName()));
                break;

            case kAutoAddScreenManualClient:
                showConfigureServer(
                    tr("The new client screen (%1) was added; link it to the other "
                        "screens to reach it.")
                        .arg(name));
                break;
            default:
                break;
            }
        }
        else if (saveServerConfig()) {
            restart_cmd_app();
        }
    }
}

bool MainWindow::reloadEditedServerConfig()
{
    // the file may have been edited by hand since; that version is the one to edit
    QString fileName = ui_->m_pLineEditConfigFile->text();
    if (QFile::exists(fileName) && serverConfig().changedOnDisk(fileName)) {
        if (!serverConfig().load(fileName, &m_ServerConfigError)) {
            QMessageBox::warning(this, tr("Configuration file has an error"),
                                 tr("%1 could not be read: %2\n\nFix it in a text editor, "
                                    "or choose another file.").arg(fileName, m_ServerConfigError));
            return false;
        }
        m_ServerConfigError.clear();
    }
    return true;
}

void MainWindow::showConfigureServer(const QString& message)
{
    QString fileName = ui_->m_pLineEditConfigFile->text();
    if (!reloadEditedServerConfig())
        return;

    ServerConfigDialog dlg(this, serverConfig(), getScreenName());
    dlg.message(message);
    if (dlg.exec() == QDialog::Accepted && saveServerConfig() && m_ExpectedRunningState == kStarted)
        appendLogInfo(tr("saved %1; the running server applies it within a few seconds").arg(fileName));
}

void MainWindow::on_m_pButtonConfigureServer_clicked()
{
    showConfigureServer();
}

void MainWindow::on_m_pButtonReload_clicked()
{
    restart_cmd_app();
}

void MainWindow::on_m_pButtonPairClient_clicked()
{
    pair(pairing::Protocol::Role::Server);
}

void MainWindow::on_m_pButtonPairServer_clicked()
{
    pair(pairing::Protocol::Role::Client);
}

void MainWindow::pair(pairing::Protocol::Role role)
{
    const bool server = role == pairing::Protocol::Role::Server;

    const auto certificate = inputleap::DataDirectories::ssl_certificate_path();
    QByteArray fingerprint;
    try {
        const auto sha256 = inputleap::get_pem_file_cert_fingerprint(
            certificate.u8string(), inputleap::FingerprintType::SHA256).data;
        fingerprint = QByteArray(reinterpret_cast<const char*>(sha256.data()), static_cast<int>(sha256.size()));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Can't pair"),
                             tr("This computer's certificate, %1, couldn't be read: %2")
                                 .arg(QString::fromStdString(certificate.u8string()), e.what()));
        return;
    }

    QStringList placed;
    if (server) {
        if (!reloadEditedServerConfig())
            return;
        for (const Screen& screen : std::as_const(m_ServerConfig).screens())
            placed << screen.name();
    }

    PairingDialog dialog(this, role, getScreenName(), fingerprint,
                         server ? getIPAddresses() : ui_->m_pLineEditHostname->text(), placed);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString peer = dialog.peerName();
    const auto db_path = server ? inputleap::DataDirectories::trusted_clients_ssl_fingerprints_path()
                                : inputleap::DataDirectories::trusted_servers_ssl_fingerprints_path();
    inputleap::fs::create_directories(db_path.parent_path());
    inputleap::FingerprintDatabase db;
    db.read(db_path);
    const QByteArray peer_fingerprint = dialog.peerFingerprint();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(peer_fingerprint.constData());
    const inputleap::FingerprintData trusted{
        inputleap::fingerprint_type_to_string(inputleap::FingerprintType::SHA256),
        std::vector<std::uint8_t>(bytes, bytes + peer_fingerprint.size())};
    // pairing again with a computer this one already trusts changes nothing
    const bool new_peer = !db.is_trusted(trusted);
    if (new_peer) {
        db.add_trusted(trusted);
        db.write(db_path);
        appendLogInfo(QString("paired with %1, adding it to %2")
                          .arg(peer, QString::fromStdString(db_path.u8string())));
    }
    inputleap::remember_computer_name(trusted, peer.toStdString());
    setStatus(tr("Paired with %1.").arg(peer));
    updatePairedComputers();

    if (!server) {
        // the client gave up on a server it didn't trust, so connect again now rather than
        // after it backs off
        if (new_peer && m_ExpectedRunningState == kStarted)
            restart_cmd_app();
        return;
    }

    // the server reads its layout again once the file changes
    if (!reloadEditedServerConfig())
        return;
    m_ServerConfig.setServerName(getScreenName());
    m_ServerConfig.ensureServerScreen(QString());
    bool linked = true;
    if (!m_ServerConfig.hasScreen(peer))
        linked = m_ServerConfig.addScreenBeside(peer, dialog.side());
    if (!saveServerConfig())
        return;
    if (!linked) {
        showConfigureServer(tr("%1 was added, but the %2 edge of this screen already leads to another "
                               "screen. Link %1 to the other screens to reach it.")
                                .arg(peer, ScreenLink::sideLabel(dialog.side())));
    }
}

void MainWindow::updatePairedComputers()
{
    const auto folder = QString::fromStdString(inputleap::DataDirectories::ssl_fingerprints_path().u8string());
    if (!m_PairedWatcher.directories().contains(folder) && QFileInfo::exists(folder)) {
        if (!m_PairedWatcher.directories().isEmpty())
            m_PairedWatcher.removePaths(m_PairedWatcher.directories());
        m_PairedWatcher.addPath(folder);
    }

    const bool server = app_role() == AppRole::Server;
    const auto db_path = server ? inputleap::DataDirectories::trusted_clients_ssl_fingerprints_path()
                                : inputleap::DataDirectories::trusted_servers_ssl_fingerprints_path();
    inputleap::FingerprintDatabase db;
    db.read(db_path);
    inputleap::PairedNames names;
    names.read(inputleap::DataDirectories::paired_names_path());

    // only these decide who is trusted
    std::vector<inputleap::FingerprintData> trusted;
    if (m_AppConfig->getCryptoEnabled() && (!serviceMode() || !m_ServiceSettings.role.empty())) {
        const char* sha256 = inputleap::fingerprint_type_to_string(inputleap::FingerprintType::SHA256);
        for (const auto& fingerprint : db.fingerprints()) {
            if (fingerprint.algorithm == sha256)
                trusted.push_back(fingerprint);
        }
    }

    // a client paired before names were kept can still say which server it uses
    QString only_server;
    if (!server && trusted.size() == 1) {
        const QString address = serviceMode() ? QString::fromStdString(m_ServiceSettings.server_address) : hostname();
        const TailscalePeer* peer = find_tailscale_peer(m_TailscalePeers, address);
        only_server = peer != nullptr ? peer->name : pairing::host_of(address);
    }

    const auto rows = ui_->m_pWidgetPaired->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* row : rows) {
        // later, since this may be running because of a button in one of them
        row->hide();
        row->deleteLater();
    }

    QMap<QString, QString> labels;
    for (const auto& fingerprint : trusted) {
        const QString name = QString::fromStdString(names.name_of(fingerprint));
        const QByteArray bytes(reinterpret_cast<const char*>(fingerprint.data.data()),
                               static_cast<int>(fingerprint.data.size()));
        const QString label = pairing::computer_label(name, only_server, bytes);
        labels.insert(QString::fromStdString(inputleap::FingerprintDatabase::to_db_line(fingerprint)), label);

        auto* row = new QWidget(ui_->m_pWidgetPaired);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* text = new QLabel(tr("Paired with %1.").arg(label), row);
        text->setTextFormat(Qt::PlainText);
        layout->addWidget(text);
        layout->addStretch();
        auto* button = new QPushButton(tr("Unpair %1...").arg(label), row);
        button->setAutoDefault(false);
        connect(button, &QPushButton::clicked, this, [this, fingerprint, name, label]() {
            unpair(fingerprint, name, label);
        });
        layout->addWidget(button);
        ui_->m_pLayoutPaired->addWidget(row);
    }
    ui_->m_pWidgetPaired->setVisible(!trusted.empty());

    // the other computer unpaired this one, or it was taken out of the list by hand
    if (server == m_PairedAsServer) {
        for (auto it = m_PairedLabels.cbegin(); it != m_PairedLabels.cend(); ++it) {
            if (!labels.contains(it.key()))
                setStatus(tr("%1 is no longer paired with this computer.").arg(it.value()));
        }
    }
    m_PairedLabels = labels;
    m_PairedAsServer = server;
}

void MainWindow::unpair(const inputleap::FingerprintData& fingerprint, const QString& name, const QString& label)
{
    const bool server = app_role() == AppRole::Server;

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setTextFormat(Qt::PlainText);
    box.setWindowTitle(tr("Unpair %1").arg(label));
    box.setText(tr("Unpair %1?").arg(label));
    box.setInformativeText(
        (server ? tr("%1 can't connect to this computer any more, and comes off this computer's layout. ").arg(label)
                : tr("This computer can't connect to %1 any more. ").arg(label)) +
        tr("To use them together again, pair them again with a new code on both.\n\n"
           "If %1 is connected now, it forgets this computer too. If it isn't, it keeps this computer "
           "on its list until you unpair it there as well, but the two can't connect.").arg(label));
    auto* confirm = box.addButton(tr("&Unpair %1").arg(label), QMessageBox::DestructiveRole);
    box.setDefaultButton(box.addButton(QMessageBox::Cancel));
    box.exec();
    if (box.clickedButton() != confirm)
        return;

    const auto db_path = server ? inputleap::DataDirectories::trusted_clients_ssl_fingerprints_path()
                                : inputleap::DataDirectories::trusted_servers_ssl_fingerprints_path();
    if (!inputleap::forget_computer(db_path, fingerprint)) {
        QMessageBox::warning(this, tr("Can't unpair"),
                             tr("%1 couldn't be changed, so this computer still trusts %2.")
                                 .arg(QString::fromStdString(db_path.u8string()), label));
        return;
    }
    appendLogInfo(QString("unpaired %1, taking it out of %2").arg(label, QString::fromStdString(db_path.u8string())));

    // says it in its own words, not as something that happened to this computer
    m_PairedLabels.remove(QString::fromStdString(inputleap::FingerprintDatabase::to_db_line(fingerprint)));
    updatePairedComputers();
    setStatus(tr("Unpaired %1.").arg(label));

    // only after the list above, so that the running server tells it before it comes off the layout
    if (server && !name.isEmpty() && reloadEditedServerConfig()) {
        m_ServerConfig.setServerName(getScreenName());
        if (m_ServerConfig.takeOffLayout(name))
            saveServerConfig();
    }
}

#if defined(Q_OS_WIN)
bool MainWindow::isServiceRunning(QString name)
{
    SC_HANDLE hSCManager;
    hSCManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (hSCManager == nullptr) {
        appendLogError("failed to open a service controller manager, error: " +
            GetLastError());
        return false;
    }

    auto array = name.toLocal8Bit();

#if QT_VERSION_MAJOR < 6
    SC_HANDLE hService = OpenService(hSCManager, array.data(), SERVICE_QUERY_STATUS);
#else
    SC_HANDLE hService = OpenService(hSCManager, reinterpret_cast<LPCWSTR>(array.data()), SERVICE_QUERY_STATUS);
#endif
    if (hService == nullptr) {
        appendLogDebug("failed to open service: " + name);
        return false;
    }

    SERVICE_STATUS status;
    if (QueryServiceStatus(hService, &status)) {
        if (status.dwCurrentState == SERVICE_RUNNING) {
            return true;
        }
    }

    return false;
}
#else
bool MainWindow::isServiceRunning()
{
    return false;
}
#endif

bool MainWindow::isBonjourRunning()
{
    bool result = false;

#if defined(Q_OS_WIN)
    result = isServiceRunning("Bonjour Service");
#else
    result = true;
#endif

    return result;
}

void MainWindow::downloadBonjour()
{
#if defined(Q_OS_WIN)
    QUrl url;
    int arch = getProcessorArch();
    if (arch == kProcessorArchWin32) {
        url.setUrl(bonjourBaseUrl + bonjourFilename32);
        appendLogInfo("downloading 32-bit Bonjour");
    }
    else if (arch == kProcessorArchWin64) {
        url.setUrl(bonjourBaseUrl + bonjourFilename64);
        appendLogInfo("downloading 64-bit Bonjour");
    }
    else {
        QMessageBox::critical(
            this, tr("Leapdesk KVM"),
            tr("Failed to detect system architecture."));
        return;
    }

    if (m_pDataDownloader == nullptr) {
        m_pDataDownloader = new DataDownloader(this);
        connect(m_pDataDownloader, &DataDownloader::isComplete, this, &MainWindow::installBonjour);
    }

    m_pDataDownloader->download(url);

    if (m_DownloadMessageBox == nullptr) {
        m_DownloadMessageBox = new QMessageBox(this);
        m_DownloadMessageBox->setWindowTitle("Leapdesk KVM");
        m_DownloadMessageBox->setIcon(QMessageBox::Information);
        m_DownloadMessageBox->setText("Installing Bonjour, please wait...");
#if QT_VERSION_MAJOR < 6
        m_DownloadMessageBox->setStandardButtons(0);
#else
        m_DownloadMessageBox->setStandardButtons(QMessageBox::NoButton);
#endif
        m_pCancelButton = m_DownloadMessageBox->addButton(
            tr("Cancel"), QMessageBox::RejectRole);
    }
    m_DownloadMessageBox->exec();

    if (m_DownloadMessageBox->clickedButton() == m_pCancelButton) {
        m_pDataDownloader->cancel();
    }
#endif
}

void MainWindow::installBonjour()
{
#if defined(Q_OS_WIN)
#if QT_VERSION >= 0x050000
    QString tempLocation = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
#else
    QString tempLocation = QDesktopServices::storageLocation(
                                QDesktopServices::TempLocation);
#endif
    QString filename = tempLocation;
    filename.append("\\").append(bonjourTargetFilename);
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly)) {
        m_DownloadMessageBox->hide();

        QMessageBox::warning(
            this, "Leapdesk KVM",
            tr("Failed to download Bonjour installer to location: %1")
            .arg(tempLocation));
        return;
    }

    file.write(m_pDataDownloader->data());
    file.close();

    QStringList arguments;
    arguments.append("/i");
    QString winFilename = QDir::toNativeSeparators(filename);
    arguments.append(winFilename);
    arguments.append("/passive");
    if (m_BonjourInstall == nullptr) {
        m_BonjourInstall = new CommandProcess("msiexec", arguments);
    }

    QThread* thread = new QThread;
    connect(m_BonjourInstall, &CommandProcess::finished, this, &MainWindow::bonjourInstallFinished);
    connect(m_BonjourInstall, &CommandProcess::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);

    m_BonjourInstall->moveToThread(thread);
    thread->start();

    QMetaObject::invokeMethod(m_BonjourInstall, "run", Qt::QueuedConnection);

    m_DownloadMessageBox->hide();
#endif
}

void MainWindow::promptAutoConfig()
{
    if (!isBonjourRunning()) {
        int r = QMessageBox::question(
            this, tr("Leapdesk KVM"),
            tr("Do you want to enable auto config and install Bonjour?\n\n"
               "This feature helps you establish the connection."),
            QMessageBox::Yes | QMessageBox::No);

        if (r == QMessageBox::Yes) {
            m_AppConfig->setAutoConfig(true);
            downloadBonjour();
        }
        else {
            m_AppConfig->setAutoConfig(false);
            ui_->m_pCheckBoxAutoConfig->setChecked(false);
        }
    }

    m_AppConfig->setAutoConfigPrompted(true);
}

void MainWindow::comboServerList_currentIndexChanged(QString )
{
    if (ui_->m_pComboServerList->count() != 0) {
        restart_cmd_app();
    }
}

void MainWindow::on_m_pCheckBoxAutoConfig_toggled(bool checked)
{
    if (!isBonjourRunning() && checked) {
        if (!m_SuppressAutoConfigWarning) {
            int r = QMessageBox::information(
                this, tr("Leapdesk KVM"),
                tr("Auto config feature requires Bonjour.\n\n"
                   "Do you want to install Bonjour?"),
                QMessageBox::Yes | QMessageBox::No);

            if (r == QMessageBox::Yes) {
                downloadBonjour();
            }
        }

        ui_->m_pCheckBoxAutoConfig->setChecked(false);
        return;
    }

    ui_->m_pLineEditHostname->setDisabled(checked);
    appConfig().setAutoConfig(checked);
    updateZeroconfService();

    if (!checked) {
        ui_->m_pComboServerList->clear();
        ui_->m_pComboServerList->hide();
    }
}

void MainWindow::bonjourInstallFinished()
{
    appendLogInfo("Bonjour install finished");

    ui_->m_pCheckBoxAutoConfig->setChecked(true);
}

void MainWindow::windowStateChanged()
{
    if (windowState() == Qt::WindowMinimized && appConfig().getMinimizeToTray())
        hide();
}

void MainWindow::showLogWindow()
{
    m_pLogWindow->show();
}
