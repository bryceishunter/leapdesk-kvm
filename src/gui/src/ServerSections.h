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

#include "WindowParts.h"

class QCheckBox;
class QGridLayout;
class QSpinBox;
class ServerConfig;

// The sections of the main window that edit the server's layout file. Each change goes
// straight into the configuration they're given, and changed() asks for it to be saved.

// The server's options: switching, dead corners, the clipboard and the rest.
class ServerOptionsSection : public CollapsibleSection
{
    Q_OBJECT

public:
    explicit ServerOptionsSection(QWidget* parent = nullptr);
    void setConfig(ServerConfig* config);
    // shows the configuration's options again, after it was loaded from the file say
    void refresh();

signals:
    void changed();

protected:
    void changeEvent(QEvent* event) override;

private:
    void build();
    void store();

    ServerConfig* m_Config = nullptr;
    bool m_Loading = false;

    QCheckBox* m_SwitchDelay = nullptr;
    QSpinBox* m_SwitchDelayTime = nullptr;
    QCheckBox* m_DoubleTap = nullptr;
    QSpinBox* m_DoubleTapTime = nullptr;
    QCheckBox* m_Corners[4] = {};
    QSpinBox* m_CornerSize = nullptr;
    QCheckBox* m_Clipboard = nullptr;
    QSpinBox* m_ClipboardSize = nullptr;
    QCheckBox* m_RelativeMoves = nullptr;
    QCheckBox* m_ScreenSaverSync = nullptr;
    QCheckBox* m_KeepForeground = nullptr;
    QCheckBox* m_Heartbeat = nullptr;
    QSpinBox* m_HeartbeatTime = nullptr;
};

// The hotkeys, each with what it does, and the rules the window can't edit.
class HotkeysSection : public CollapsibleSection
{
    Q_OBJECT

public:
    explicit HotkeysSection(QWidget* parent = nullptr);
    void setConfig(ServerConfig* config);
    void refresh();

signals:
    void changed();

protected:
    void changeEvent(QEvent* event) override;

private:
    void build();
    // edits the hotkey at index, or adds one if it's -1
    void edit(int index);
    void remove(int index);

    ServerConfig* m_Config = nullptr;
    QWidget* m_List = nullptr;
    QWidget* m_Preserved = nullptr;
};
