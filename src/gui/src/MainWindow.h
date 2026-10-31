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

#pragma once

#include "inputleap/AppRole.h"
#include "inputleap/ServiceSettings.h"
#include "net/FingerprintData.h"
#include "AppConnectionState.h"
#include "ConnectionStatus.h"
#include "Pairing.h"
#include "TailscalePeers.h"
#include "WindowParts.h"

#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QMap>
#include <QSystemTrayIcon>
#include <QSettings>
#include <QProcess>
#include <QThread>
#include <QTimer>



#include "ServerConfig.h"
#include "AppConfig.h"
#include "IpcClient.h"
#include "Ipc.h"
#include "LogWindow.h"

#include <QMutex>
#include <memory>

class QAction;
class QGridLayout;
class QVBoxLayout;
class QMenu;
class QLineEdit;
class QGroupBox;
class QPushButton;
class QTextEdit;
class QComboBox;
class QTabWidget;
class QCheckBox;
class QRadioButton;
class QMessageBox;
class QAbstractButton;

class LogDialog;
class QInputLeapApplication;
class SetupWizard;
class ZeroconfService;
class DataDownloader;
class CommandProcess;
class SslCertificate;

namespace Ui
{
    class MainWindow;
}
class MainWindow : public QMainWindow
{
    Q_OBJECT

    friend class QInputLeapApplication;
    friend class SetupWizard;

    public:
        enum qLevel {
            Error,
            Info
        };

        enum qRuningState {
            kStarted,
            kStopped
        };

    public:
        MainWindow(QSettings& settings, AppConfig& appConfig);
        ~MainWindow() override;

    public:
        void setVisible(bool visible) override;
        AppRole app_role() const;
        AppConnectionState connection_state() const { return connection_state_; }
        QString hostname() const;
        QString configFilename();
        QString address();
        QString appPath(const QString& name);
        void open();
        QString getScreenName();
        ServerConfig& serverConfig() { return m_ServerConfig; }
        void showConfigureServer(const QString& message);
        void showConfigureServer() { showConfigureServer(""); }
        void autoAddScreen(const QString name);
        void updateZeroconfService();
        void serverDetected(const QString name);

    Q_SIGNALS:
        void requestLanguageChange(QString newLanguage);

public slots:
        void appendLogRaw(const QString& text);
        void appendLogInfo(const QString& text);
        void appendLogDebug(const QString& text);
        void appendLogError(const QString& text);
        void start_cmd_app();
        void setServerMode(bool isServerMode);

    protected slots:
        void on_m_pCardClient_toggled(bool on);
        void on_m_pCardServer_toggled(bool on);
        bool on_m_pButtonBrowseConfigFile_clicked();
        void on_m_pButtonConfigureServer_clicked();
        void on_m_pButtonApply_clicked();
        void on_m_pButtonUndo_clicked();
        void on_m_pButtonTailscale_clicked();
        void on_m_pButtonPairClient_clicked();
        void on_m_pButtonPairServer_clicked();
        bool on_m_pActionSave_triggered();
        void on_m_pActionAbout_triggered();
        void on_m_pActionSettings_triggered();
        void cmd_app_finished(int exitCode, QProcess::ExitStatus);
        void trayActivated(QSystemTrayIcon::ActivationReason reason);
        void stop_cmd_app();
        void logOutput();
        void logError();
        void bonjourInstallFinished();
        void showLogWindow();

    protected:
        QSettings& settings() { return m_Settings; }
        AppConfig& appConfig() { return *m_AppConfig; }
        void initConnections();
        void createMenuBar();
        void createTrayIcon();
        void loadSettings();
        void loadServerConfig();
        // starts or stops the Leapdesk service, asking for administrator approval if needed;
        // a start waits up to tries half-seconds for a stop that is still going on
        void controlService(bool start, int tries = 60);
        // whether this window manages the Leapdesk service rather than a process of its own
        bool serviceMode() const { return m_AppConfig->processMode() == Service; }
        // hides what only applies to one way of running, and labels the rest for it
        void adaptToProcessMode();
        // shows what HKLM\SOFTWARE\Leapdesk holds, dropping any changes not applied
        void loadServiceSettings();
        // the service settings as this window now has them
        inputleap::ServiceSettings draftServiceSettings() const;
        // offers Apply when the window differs from the service settings
        void updateApplyBar();
        // runs `tailscale status`, then updates the network path and shows the devices if asked
        void refreshTailscale(bool showDevices);
        void showTailscaleDevices();
        void updateServerPath();
        // pairs with a client as the server, or with the server as a client, and trusts it
        void pair(pairing::Protocol::Role role);
        // the role the sections below "This computer" are for: the one the service runs, or
        // without the service, the one chosen
        AppRole shownRole() const;
        bool hasShownRole() const;
        // shows the sections for the role, and what's in them
        void updateSections();
        // the line across the top: what's running, and whether it's connected
        void updateStatus();
        // the Connection section: the computers this one is paired with, each with a button to
        // unpair it, and the facts about this one
        void updatePairedComputers();
        void updateConnection();
        // the Screens section, until the layout can be edited in the window
        void updateLayoutSummary();
        // the computers this one trusts, as they're shown
        struct PairedComputer {
            inputleap::FingerprintData fingerprint;
            QString name;   // what it was called when paired, or empty
            QString label;  // what it's called here
        };
        QList<PairedComputer> pairedComputers() const;
        // what the server this client uses is called
        QString serverLabel() const;
        // why this client can't connect, from what it logged
        QString failureText() const;
        // a fact in the Connection section, such as "Encryption"; returns the label holding text
        QLabel* addFact(const QString& name, const QString& text);
        // a line saying whether something is right, with what to do if it isn't
        void addCheck(Tone tone, const QString& title, const QString& detail, QWidget* action = nullptr);
        void addPairedTable(const QList<PairedComputer>& paired);
        void addCertificateFact();
        void addCopyAndPasteFact();
        void connectionLinkActivated(const QString& link);
        // this computer's IPv4 addresses, those on Tailscale first
        QStringList tailscaleAddresses() const;
        QStringList otherAddresses() const;
        // where clients reach this server
        QString serverAddress() const;
        // is the service running, as far as this window can tell
        bool isRunning() const;
        // asks, then stops trusting the computer with this fingerprint; its running server or
        // client notices, and tells that computer if it's connected
        void unpair(const inputleap::FingerprintData& fingerprint, const QString& name, const QString& label);
        // loads the configuration file again if it was edited elsewhere; false if it can't be read
        bool reloadEditedServerConfig();
        bool saveServerConfig();
        void saveSettings();
        void set_icon(AppConnectionState state);
        void set_connection_state(AppConnectionState state);
        bool clientArgs(QStringList& args, QString& app);
        bool serverArgs(QStringList& args, QString& app);
        void setStatus(const QString& status);
        void updateFromLogLine(const QString& line);
        QString getIPAddresses();
        void stopService();
        void stopDesktop();
        void changeEvent(QEvent* event) override;
        void retranslateMenuBar();
#if defined(Q_OS_WIN)
        bool isServiceRunning(QString name);
#else
        bool isServiceRunning();
#endif
        bool isBonjourRunning();
        void downloadBonjour();
        void promptAutoConfig();
        void checkConnected(const QString& line);
        void checkFingerprint(const QString& line);
        void restart_cmd_app();
        void proofreadInfo();
        void windowStateChanged();
        void updateSSLFingerprint();

    private:
        std::unique_ptr<Ui::MainWindow> ui_;
        QSettings& m_Settings;
        AppConfig* m_AppConfig;
        QProcess* cmd_app_process_;
        AppConnectionState connection_state_ = AppConnectionState::DISCONNECTED;
        ServerConfig m_ServerConfig;
        // why the configuration file could not be read, empty if it could
        QString m_ServerConfigError;
        QSystemTrayIcon* m_pTrayIcon;
        QMenu* m_pTrayIconMenu;
        bool m_AlreadyHidden;
        IpcClient m_IpcClient;
        QMenuBar* m_pMenuBar;
        QMenu* main_menu_;
        QMenu* m_pMenuHelp;
        ZeroconfService* m_pZeroconfService;
        DataDownloader* m_pDataDownloader;
        QMessageBox* m_DownloadMessageBox;
        QAbstractButton* m_pCancelButton;
        QMutex m_UpdateZeroconfMutex;
        bool m_SuppressAutoConfigWarning;
        CommandProcess* m_BonjourInstall;
        bool m_SuppressEmptyServerWarning;
        qRuningState m_ExpectedRunningState;
        QMutex m_StopDesktopMutex;
        SslCertificate* m_pSslCertificate;
        QStringList m_PendingClientNames;
        LogWindow *m_pLogWindow;

        bool m_fingerprint_expanded = false;
        // this computer's certificate, as the Connection section shows it
        QString m_Sha256Short;
        QString m_Sha256Full;
        QString m_Sha1Full;
        QString m_Randomart;
        // where the Connection section is being filled in
        QGridLayout* m_pFacts = nullptr;
        QVBoxLayout* m_pChecks = nullptr;
        // what the running server or client has said
        ConnectionStatus m_Connection;
        // something that just happened, shown under the status line until the status changes
        QString m_StatusNotice;
        QString m_StatusShown;
        QTimer m_ServicePoll;
        inputleap::ServiceSettings m_ServiceSettings;
        QList<TailscalePeer> m_TailscalePeers;
        QString m_TailscaleName;
        QProcess* m_TailscaleProcess = nullptr;
        bool m_ShowTailscaleDevices = false;
        QFileSystemWatcher m_PairedWatcher;
        // what each paired computer shown is called, by the line for it in the list of trusted
        // computers, so that one gone from the list can be named
        QMap<QString, QString> m_PairedLabels;
        bool m_PairedAsServer = false;

private slots:
    void on_m_pCheckBoxAutoConfig_toggled(bool checked);
    void comboServerList_currentIndexChanged(QString );
    void on_m_pButtonReload_clicked();
    void installBonjour();

};
