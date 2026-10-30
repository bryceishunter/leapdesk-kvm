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

#include "PairingDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// how soon a client tries again when the server wasn't waiting to pair
constexpr int kRetryMilliseconds = 2000;

// a reason as a sentence, since socket errors come without a full stop
QString sentence(const QString& reason)
{
    return reason.endsWith('.') ? reason : reason + '.';
}

} // namespace

PairingDialog::PairingDialog(QWidget* parent, pairing::Protocol::Role role, const QString& name,
                             const QByteArray& fingerprint, const QString& address, const QStringList& placed) :
    QDialog(parent),
    m_Role(role),
    m_Name(name),
    m_Fingerprint(fingerprint),
    m_Address(address),
    m_Placed(placed),
    m_pMessage(new QLabel(this)),
    m_pCode(new QLabel(this)),
    m_pPlace(new QWidget(this)),
    m_pSide(new QComboBox(m_pPlace)),
    m_pButtonPair(new QPushButton(tr("&Pair"), this))
{
    setWindowTitle(isServer() ? tr("Pair a Computer")
                              : tr("Pair with %1").arg(pairing::host_of(address)));
    setMinimumWidth(460);

    m_pMessage->setWordWrap(true);
    m_pMessage->setTextFormat(Qt::RichText);

    QFont font = m_pCode->font();
    font.setPointSizeF(font.pointSizeF() * 2.5);
    font.setBold(true);
    m_pCode->setFont(font);
    m_pCode->setAlignment(Qt::AlignCenter);
    m_pCode->hide();

    auto* label = new QLabel(tr("P&lace it:"), m_pPlace);
    label->setBuddy(m_pSide);
    m_pSide->addItem(tr("Right of this screen"), static_cast<int>(ScreenLink::Side::Right));
    m_pSide->addItem(tr("Left of this screen"), static_cast<int>(ScreenLink::Side::Left));
    m_pSide->addItem(tr("Above this screen"), static_cast<int>(ScreenLink::Side::Up));
    m_pSide->addItem(tr("Below this screen"), static_cast<int>(ScreenLink::Side::Down));
    auto* place = new QHBoxLayout(m_pPlace);
    place->setContentsMargins(0, 0, 0, 0);
    place->addWidget(label);
    place->addWidget(m_pSide);
    place->addStretch();
    m_pPlace->hide();

    // pairing takes a deliberate click, never Enter
    m_pButtonPair->setAutoDefault(false);
    m_pButtonPair->setEnabled(false);
    connect(m_pButtonPair, &QPushButton::clicked, this, &PairingDialog::pairClicked);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->addButton(m_pButtonPair, QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_pMessage);
    layout->addWidget(m_pCode);
    layout->addWidget(m_pPlace);
    layout->addStretch();
    layout->addWidget(buttons);

    if (isServer()) {
        listen();
    } else {
        m_pMessage->setText(tr("Connecting to %1...").arg(pairing::host_of(m_Address).toHtmlEscaped()));
        connectToServer();
    }
}

PairingDialog::~PairingDialog() = default;

ScreenLink::Side PairingDialog::side() const
{
    return static_cast<ScreenLink::Side>(m_pSide->currentData().toInt());
}

void PairingDialog::listen()
{
    m_pServer = new QTcpServer(this);
    connect(m_pServer, &QTcpServer::newConnection, this, &PairingDialog::acceptConnections);
    if (!m_pServer->listen(QHostAddress::Any, pairing::kPort)) {
        m_pMessage->setText(tr("Can't wait for a computer to pair, since port %1 isn't free: %2")
                                .arg(pairing::kPort)
                                .arg(m_pServer->errorString().toHtmlEscaped()));
        return;
    }
    showWaiting(QString());
}

void PairingDialog::acceptConnections()
{
    while (QTcpSocket* socket = m_pServer->nextPendingConnection()) {
        if (m_pConnection != nullptr) {
            pairing::Connection::refuse(socket);
            continue;
        }
        m_pConnection = new pairing::Connection(socket, pairing::Protocol(m_Role, m_Name, m_Fingerprint), this);
        connect(m_pConnection, &pairing::Connection::changed, this, &PairingDialog::update);
    }
}

void PairingDialog::connectToServer()
{
    auto* socket = new QTcpSocket(this);
    m_pConnection = new pairing::Connection(socket, pairing::Protocol(m_Role, m_Name, m_Fingerprint), this);
    connect(m_pConnection, &pairing::Connection::changed, this, &PairingDialog::update);
    socket->connectToHost(pairing::host_of(m_Address), pairing::kPort);
}

void PairingDialog::showWaiting(const QString& before)
{
    QString text = tr("Waiting for a computer to pair.<br><br>On that computer, open Leapdesk KVM, choose "
                      "Client with this computer's address, press Apply, then press Pair with Server."
                      "<br><br>This computer's addresses: %1").arg(m_Address);
    if (!before.isEmpty())
        text = before + "<br><br>" + text;
    m_pMessage->setText(text);
    m_pCode->hide();
    m_pPlace->hide();
    m_pButtonPair->setEnabled(false);
}

void PairingDialog::update()
{
    if (m_pConnection == nullptr)
        return;

    const pairing::Protocol& protocol = m_pConnection->protocol();
    const QString peer = protocol.peer_name().toHtmlEscaped();
    const QString host = pairing::host_of(m_Address).toHtmlEscaped();

    switch (protocol.state()) {
    case pairing::Protocol::State::Starting:
        break;

    case pairing::Protocol::State::Exchanging:
        if (!isServer())
            m_pMessage->setText(tr("Connected to %1.").arg(host));
        break;

    case pairing::Protocol::State::Comparing:
        m_pCode->setText(pairing::spaced(protocol.code()));
        m_pCode->show();
        m_pMessage->setText(protocol.confirmed_here()
            ? tr("Waiting for someone to press Pair on %1...").arg(peer)
            : tr("Check that %1 shows this code too, and press Pair only if it does.").arg(peer));
        m_pPlace->setVisible(isServer() && !m_Placed.contains(protocol.peer_name(), Qt::CaseInsensitive));
        m_pButtonPair->setEnabled(!protocol.confirmed_here());
        break;

    case pairing::Protocol::State::Paired:
        m_PeerName = protocol.peer_name();
        m_PeerFingerprint = protocol.peer_fingerprint();
        accept();
        break;

    case pairing::Protocol::State::Failed: {
        const bool retry = m_pConnection->worth_retrying();
        const bool busy = protocol.peer_busy();
        const QString error = sentence(protocol.error()).toHtmlEscaped();
        m_pConnection->deleteLater();
        m_pConnection = nullptr;

        if (isServer()) {
            // something that never said hello isn't worth mentioning
            showWaiting(peer.isEmpty() ? QString() : tr("Pairing with %1 stopped. %2").arg(peer, error));
            break;
        }

        m_pCode->hide();
        if (retry) {
            m_pMessage->setText(busy
                ? tr("%1 is pairing with another computer. This keeps trying until it's done.").arg(host)
                : tr("%1 isn't waiting to pair yet. %2<br><br>On %1, open Leapdesk KVM and press "
                     "Pair a Computer. This keeps trying until then.").arg(host, error));
            m_pButtonPair->setEnabled(false);
            QTimer::singleShot(kRetryMilliseconds, this, &PairingDialog::connectToServer);
        } else {
            m_pMessage->setText(tr("Pairing with %1 stopped. %2").arg(peer.isEmpty() ? host : peer, error));
            m_pButtonPair->setText(tr("&Try Again"));
            m_pButtonPair->setEnabled(true);
        }
        break;
    }
    }
}

void PairingDialog::pairClicked()
{
    if (m_pConnection != nullptr) {
        m_pConnection->confirm();
        return;
    }

    // a client that stopped tries again
    m_pButtonPair->setText(tr("&Pair"));
    m_pButtonPair->setEnabled(false);
    m_pMessage->setText(tr("Connecting to %1...").arg(pairing::host_of(m_Address).toHtmlEscaped()));
    connectToServer();
}
