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

#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QObject>
#include <QString>

class QJsonObject;
class QTcpSocket;
class QTimer;

// Pairs a server and a client: each window learns the SHA-256 fingerprint of the other
// computer's certificate, and trusts it once the people at both computers see the same
// six-digit code.
//
// The code is a hash of both fingerprints and of a random nonce from each computer. Someone
// in between would show each side a certificate of their own, so the two hashes would cover
// different fingerprints, and they would have to pick nonces that make the codes agree anyway.
// The server sends a hash of its nonce before it sees the client's, and the client sends its
// nonce before it sees the server's, so neither can be picked that way: the codes agree by
// chance one time in a million.
namespace pairing {

// the port the server's window listens on while it waits for a computer to pair
constexpr quint16 kPort = 24802;

QByteArray new_nonce();
QByteArray commitment(const QByteArray& server_fingerprint, const QByteArray& client_fingerprint,
                      const QByteArray& server_nonce);
QString code(const QByteArray& server_fingerprint, const QByteArray& client_fingerprint,
             const QByteArray& server_nonce, const QByteArray& client_nonce);
// a code as people read it, such as "482 913"
QString spaced(const QString& code);
// the host in a server address such as 100.111.152.25:24800, [fd7a:115c::1]:24800 or desk
QString host_of(const QString& address);

// One side of the exchange, apart from the network: it takes each line the other computer
// sends and returns the lines to send back.
class Protocol
{
    Q_DECLARE_TR_FUNCTIONS(Pairing)

public:
    enum class Role { Server, Client };
    enum class State {
        Starting,   // the server waits for the client's hello
        Exchanging, // fingerprints and nonces are on their way
        Comparing,  // code() is ready for the person to compare
        Paired,     // the people at both computers said the codes match
        Failed      // error() says why; nothing more is sent
    };

    Protocol(Role role, const QString& name, const QByteArray& fingerprint,
             const QByteArray& nonce = new_nonce());

    // the client's first line
    QByteArray hello();
    // takes one line from the other computer, without its newline, and returns the lines to send
    QList<QByteArray> receive(const QByteArray& line);
    // the person here saw the same code on both computers; returns the line to send
    QByteArray confirm();
    // stops, unless it has finished
    void abandon(const QString& error);

    // the line a server sends a computer that wants to pair while it pairs with another
    static QByteArray busy();

    Role role() const { return role_; }
    State state() const { return state_; }
    QString error() const { return error_; }
    // whether the other computer was pairing with someone else, so trying again later may work
    bool peer_busy() const { return peer_busy_; }
    QString code() const { return code_; }
    QString peer_name() const { return peer_name_; }
    QByteArray peer_fingerprint() const { return peer_fingerprint_; }
    bool confirmed_here() const { return confirmed_here_; }
    bool confirmed_there() const { return confirmed_there_; }

private:
    enum class Expecting { Hello, Nonce, Confirmation, Nothing };

    // stops with error; a reason other computers know is sent to tell them why
    QList<QByteArray> fail(const QString& error, const QString& reason = QString());
    bool take_hello(const QJsonObject& message, QList<QByteArray>& refusal);
    void compare();

    Role role_;
    QString name_;
    QByteArray fingerprint_;
    QByteArray nonce_;
    State state_;
    Expecting expecting_;
    QString error_;
    bool peer_busy_ = false;
    QString peer_name_;
    QByteArray peer_fingerprint_;
    QByteArray peer_commitment_;
    QByteArray peer_nonce_;
    QString code_;
    bool confirmed_here_ = false;
    bool confirmed_there_ = false;
};

// A Protocol over a TCP connection, one JSON object per line.
class Connection : public QObject
{
    Q_OBJECT

public:
    // takes over socket; a client's may still be connecting, and sends its hello once it has
    Connection(QTcpSocket* socket, const Protocol& protocol, QObject* parent = nullptr);

    const Protocol& protocol() const { return protocol_; }
    // whether it failed before reaching the other window, or found it busy, so that
    // trying again in a moment may work
    bool worth_retrying() const;
    void confirm();

    // tells a computer that wants to pair that this one is pairing with another, and closes
    static void refuse(QTcpSocket* socket);

Q_SIGNALS:
    // the protocol's state or confirmations changed
    void changed();

private:
    void read();
    void send(const QList<QByteArray>& lines);
    void stop(const QString& error);
    // closes the socket once there is nothing more to say, and tells whoever is watching
    void settle();

    QTcpSocket* socket_;
    Protocol protocol_;
    QTimer* timer_;
    bool reached_ = false;
};

} // namespace pairing
