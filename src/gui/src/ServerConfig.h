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

#include <QList>
#include <QStringList>

#include <memory>
#include <vector>

#include "Screen.h"
#include "BaseConfig.h"
#include "Hotkey.h"
#include "ScreenLink.h"

class QSettings;
class QString;
class ServerConfigDialog;
class MainWindow;

namespace inputleap { class Config; }

// The server configuration being edited. Its source of truth is a
// configuration file: load() reads everything from it, and save() writes back
// only what was changed here, merged into the file as loaded. Whatever this
// class does not model (comments aside) therefore survives a save untouched.
class ServerConfig : public BaseConfig
{
    friend class ServerConfigDialog;

    public:
        ServerConfig(QSettings* settings, QString serverName, MainWindow* mainWindow);
        ~ServerConfig();

    public:
        const std::vector<Screen>& screens() const { return m_Screens; }
        const std::vector<ScreenLink>& links() const { return m_Links; }
        bool hasHeartbeat() const { return m_HasHeartbeat; }
        int heartbeat() const { return m_Heartbeat; }
        bool relativeMouseMoves() const { return m_RelativeMouseMoves; }
        bool screenSaverSync() const { return m_ScreenSaverSync; }
        bool win32KeepForeground() const { return m_Win32KeepForeground; }
        bool hasSwitchDelay() const { return m_HasSwitchDelay; }
        int switchDelay() const { return m_SwitchDelay; }
        bool hasSwitchDoubleTap() const { return m_HasSwitchDoubleTap; }
        int switchDoubleTap() const { return m_SwitchDoubleTap; }
        bool switchCorner(SwitchCorner c) const { return m_SwitchCorners[static_cast<int>(c)]; }
        int switchCornerSize() const { return m_SwitchCornerSize; }
        const QList<bool>& switchCorners() const { return m_SwitchCorners; }
        const std::vector<Hotkey>& hotkeys() const { return m_Hotkeys; }
        // hotkey rules from the file that the hotkey editor can't represent;
        // they are kept as they are and shown read-only
        const QStringList& preservedRules() const { return m_PreservedRules; }
        bool ignoreAutoConfigClient() const { return m_IgnoreAutoConfigClient; }
        bool enableDragAndDrop() const { return m_EnableDragAndDrop; }
        bool clipboardSharing() const { return m_ClipboardSharing; }
        size_t clipboardSharingSize() const { return m_ClipboardSharingSize; }
        static size_t defaultClipboardSharingSize();
        static bool isValidScreenName(const QString& name);

        // Replaces this configuration with the contents of fileName. On failure
        // nothing changes and error says why, with the line number for syntax errors.
        bool load(const QString& fileName, QString* error);

        // Writes the configuration to fileName, keeping everything this class
        // does not edit as it was loaded, and makes that the new baseline.
        bool save(const QString& fileName, QString* error);

        // Like save() but leaves the baseline alone, for "save as" copies.
        bool exportTo(const QString& fileName, QString* error) const;

        // Whether fileName no longer holds what was last loaded or saved,
        // i.e. it was edited by hand in the meantime.
        bool changedOnDisk(const QString& fileName) const;

        // Resets to a new configuration holding just the server screen.
        void reset();

        // Imports the grid layout older versions kept in the settings instead
        // of in a file; returns false when there is none.
        bool loadLegacySettings();

        void saveSettings();
        int numScreens() const;
        int autoAddScreen(const QString name);
        const QString& serverName() const { return m_ServerName; }
        void setServerName(const QString& name) { m_ServerName = name; }

        // Makes sure the configuration has a screen for the server's name: the screen
        // called previousName takes it, with its links, or else a new one is added.
        void ensureServerScreen(const QString& previousName);

    protected:
        QSettings& settings() { return *m_pSettings; }
        std::vector<Screen>& screens() { return m_Screens; }
        std::vector<ScreenLink>& links() { return m_Links; }
        void haveHeartbeat(bool on) { m_HasHeartbeat = on; }
        void setHeartbeat(int val) { m_Heartbeat = val; }
        void setRelativeMouseMoves(bool on) { m_RelativeMouseMoves = on; }
        void setScreenSaverSync(bool on) { m_ScreenSaverSync = on; }
        void setWin32KeepForeground(bool on) { m_Win32KeepForeground = on; }
        void haveSwitchDelay(bool on) { m_HasSwitchDelay = on; }
        void setSwitchDelay(int val) { m_SwitchDelay = val; }
        void haveSwitchDoubleTap(bool on) { m_HasSwitchDoubleTap = on; }
        void setSwitchDoubleTap(int val) { m_SwitchDoubleTap = val; }
        void setSwitchCorner(SwitchCorner c, bool on) { m_SwitchCorners[static_cast<int>(c)] = on; }
        void setSwitchCornerSize(int val) { m_SwitchCornerSize = val; }
        void setIgnoreAutoConfigClient(bool on) { m_IgnoreAutoConfigClient = on; }
        void setEnableDragAndDrop(bool on) { m_EnableDragAndDrop = on; }
        void setClipboardSharing(bool on) { m_ClipboardSharing = on; }
        size_t setClipboardSharingSize(size_t size);
        QList<bool>& switchCorners() { return m_SwitchCorners; }
        std::vector<Hotkey>& hotkeys() { return m_Hotkeys; }

        // renames a screen and every link to or from it
        void renameScreen(const QString& oldName, const QString& newName);
        // removes a screen and every link to or from it
        void removeScreen(const QString& name);

    private:
        void resetOptions();
        void markLoaded();
        QString optionsText() const;
        QStringList hotkeyLines() const;
        bool applyEdits(inputleap::Config& config, QString* error) const;
        bool formatFile(QString& text, QString* error) const;
        int showAddClientDialog(const QString& clientName);

    private:
        QSettings* m_pSettings;
        std::vector<Screen> m_Screens;
        std::vector<ScreenLink> m_Links;
        bool m_HasHeartbeat;
        int m_Heartbeat;
        bool m_RelativeMouseMoves;
        bool m_ScreenSaverSync;
        bool m_Win32KeepForeground;
        bool m_HasSwitchDelay;
        int m_SwitchDelay;
        bool m_HasSwitchDoubleTap;
        int m_SwitchDoubleTap;
        int m_SwitchCornerSize;
        QList<bool> m_SwitchCorners;
        std::vector<Hotkey> m_Hotkeys;
        QStringList m_PreservedRules;
        QString m_ServerName;
        bool m_IgnoreAutoConfigClient;
        bool m_EnableDragAndDrop;
        bool m_ClipboardSharing;
        size_t m_ClipboardSharingSize;
        MainWindow* m_pMainWindow;

        // the file as last loaded or saved, which save() merges edits into
        std::shared_ptr<const inputleap::Config> m_Baseline;
        QString m_BaselineText;
        QString m_HeaderComment;

        // the editable state as it was when loaded, to find what was edited
        std::vector<Screen> m_LoadedScreens;
        std::vector<ScreenLink> m_LoadedLinks;
        QString m_LoadedOptions;
        QStringList m_LoadedHotkeys;
};

enum {
    kAutoAddScreenOk,
    kAutoAddScreenManualServer,
    kAutoAddScreenManualClient,
    kAutoAddScreenIgnore
};
