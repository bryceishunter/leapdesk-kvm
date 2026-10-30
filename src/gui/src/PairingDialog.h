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

#include "Pairing.h"
#include "ScreenLink.h"

#include <QDialog>
#include <QStringList>

class QComboBox;
class QLabel;
class QPushButton;
class QTcpServer;
class QWidget;

// Pairs this computer with another by having the people at both check the same code. A server
// waits for a computer to pair; a client pairs with the server at an address, trying again
// until that server is waiting. Accepted once both people have pressed Pair.
class PairingDialog : public QDialog
{
    Q_OBJECT

public:
    // name and fingerprint are this computer's. A server shows addresses (HTML) as where to
    // reach it, and offers a place in the layout for a computer that isn't among placed;
    // a client pairs with the server at address.
    PairingDialog(QWidget* parent, pairing::Protocol::Role role, const QString& name,
                  const QByteArray& fingerprint, const QString& address, const QStringList& placed = {});
    ~PairingDialog() override;

    QString peerName() const { return m_PeerName; }
    QByteArray peerFingerprint() const { return m_PeerFingerprint; }
    // where the server's layout should put the computer it paired with
    ScreenLink::Side side() const;

private:
    bool isServer() const { return m_Role == pairing::Protocol::Role::Server; }
    void listen();
    void acceptConnections();
    void connectToServer();
    void update();
    void showWaiting(const QString& before);
    void pairClicked();

    pairing::Protocol::Role m_Role;
    QString m_Name;
    QByteArray m_Fingerprint;
    QString m_Address;
    QStringList m_Placed;

    QLabel* m_pMessage;
    QLabel* m_pCode;
    QWidget* m_pPlace;
    QComboBox* m_pSide;
    QPushButton* m_pButtonPair;

    QTcpServer* m_pServer = nullptr;
    pairing::Connection* m_pConnection = nullptr;

    QString m_PeerName;
    QByteArray m_PeerFingerprint;
};
