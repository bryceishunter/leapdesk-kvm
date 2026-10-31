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

#include "Pairing.h"
#include "ServerConfig.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>

#include <initializer_list>

namespace pairing {

namespace {

constexpr int kVersion = 1;
// the size of each fingerprint, nonce and commitment
constexpr int kBytes = 32;
constexpr int kMaxLine = 4096;
// how long the computers may take to reach the code; people then take as long as they like
constexpr int kAnswerMilliseconds = 15000;

QByteArray hashed(const char* label, std::initializer_list<QByteArray> parts)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray(label));
    hash.addData(QByteArray(1, '\0'));
    for (const QByteArray& part : parts)
        hash.addData(part);
    return hash.result();
}

QString hex(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toHex());
}

// the kBytes bytes a message spells out in hexadecimal, or none if it holds anything else
QByteArray bytes_from_hex(const QJsonValue& value)
{
    const QString text = value.toString();
    if (text.size() != kBytes * 2)
        return {};
    for (QChar c : text) {
        const bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!digit)
            return {};
    }
    return QByteArray::fromHex(text.toLatin1());
}

bool is_screen_name(const QString& name)
{
    if (name.isEmpty() || name.size() > 255)
        return false;
    // the server's check takes bytes as they come, so only give it printable ASCII
    for (QChar c : name) {
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return false;
    }
    return ServerConfig::isValidScreenName(name);
}

QByteArray line(const QJsonObject& message)
{
    return QJsonDocument(message).toJson(QJsonDocument::Compact);
}

// why pairing stopped, as both computers tell it
QString this_computer()
{
    return Protocol::tr("That is this computer.");
}

QString other_version()
{
    return Protocol::tr("The other computer pairs in a way this version of Leapdesk KVM doesn't. "
                        "Install the same version on both.");
}

} // namespace

QByteArray new_nonce()
{
    QByteArray nonce(kBytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(nonce.data()), kBytes / 4);
    return nonce;
}

QByteArray commitment(const QByteArray& server_fingerprint, const QByteArray& client_fingerprint,
                      const QByteArray& server_nonce)
{
    return hashed("Leapdesk KVM pairing commitment", {server_fingerprint, client_fingerprint, server_nonce});
}

QString code(const QByteArray& server_fingerprint, const QByteArray& client_fingerprint,
             const QByteArray& server_nonce, const QByteArray& client_nonce)
{
    const QByteArray digest = hashed("Leapdesk KVM pairing code",
                                     {server_fingerprint, client_fingerprint, server_nonce, client_nonce});
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<quint8>(digest[i]);
    return QStringLiteral("%1").arg(value % 1000000, 6, 10, QLatin1Char('0'));
}

QString spaced(const QString& code)
{
    return code.left(3) + ' ' + code.mid(3);
}

QString host_of(const QString& address)
{
    const QString text = address.trimmed();
    if (text.startsWith('['))
        return text.mid(1, text.indexOf(']') - 1);
    if (text.count(':') == 1)
        return text.section(':', 0, 0);
    return text;
}

QString computer_label(const QString& name, const QString& only_server, const QByteArray& fingerprint)
{
    if (!name.isEmpty())
        return name;
    if (!only_server.isEmpty())
        return only_server;
    return Protocol::tr("computer %1").arg(QString::fromLatin1(fingerprint.left(4).toHex(':')));
}

Protocol::Protocol(Role role, const QString& name, const QByteArray& fingerprint, const QByteArray& nonce) :
    role_(role),
    name_(name),
    fingerprint_(fingerprint),
    nonce_(nonce),
    state_(State::Starting),
    expecting_(role == Role::Server ? Expecting::Hello : Expecting::Nothing)
{
}

QByteArray Protocol::hello()
{
    if (role_ != Role::Client || state_ != State::Starting)
        return {};
    state_ = State::Exchanging;
    expecting_ = Expecting::Hello;
    return line({{"pairing", kVersion}, {"name", name_}, {"fingerprint", hex(fingerprint_)}});
}

QByteArray Protocol::busy()
{
    return line({{"error", "busy"}});
}

QList<QByteArray> Protocol::receive(const QByteArray& text)
{
    if (state_ == State::Failed || state_ == State::Paired)
        return {};

    const QString garbled = tr("The other computer sent something that isn't Leapdesk KVM pairing.");
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(text, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return fail(garbled);
    const QJsonObject message = document.object();

    if (message.contains("error")) {
        const QString reason = message.value("error").toString();
        peer_busy_ = reason == "busy";
        if (peer_busy_)
            return fail(tr("It's pairing with another computer."));
        if (reason == "self")
            return fail(this_computer());
        if (reason == "version")
            return fail(other_version());
        return fail(tr("The other computer stopped pairing."));
    }

    switch (expecting_) {
    case Expecting::Hello: {
        QList<QByteArray> refusal;
        if (!take_hello(message, refusal))
            return refusal;
        state_ = State::Exchanging;
        expecting_ = Expecting::Nonce;
        if (role_ == Role::Server) {
            // a promise of the nonce, which the client can't then answer with a nonce to suit it
            return {line({{"pairing", kVersion}, {"name", name_}, {"fingerprint", hex(fingerprint_)},
                          {"commitment", hex(commitment(fingerprint_, peer_fingerprint_, nonce_))}})};
        }
        peer_commitment_ = bytes_from_hex(message.value("commitment"));
        if (peer_commitment_.isEmpty())
            return fail(garbled);
        return {line({{"nonce", hex(nonce_)}})};
    }

    case Expecting::Nonce:
        peer_nonce_ = bytes_from_hex(message.value("nonce"));
        if (peer_nonce_.isEmpty())
            return fail(garbled);
        if (role_ == Role::Server) {
            compare();
            return {line({{"nonce", hex(nonce_)}})};
        }
        if (commitment(peer_fingerprint_, fingerprint_, peer_nonce_) != peer_commitment_)
            return fail(tr("The other computer's answer didn't match what it promised, so it may not be "
                           "the computer you think. Nothing was paired."));
        compare();
        return {};

    case Expecting::Confirmation:
        if (!message.value("confirm").toBool(false))
            return fail(garbled);
        confirmed_there_ = true;
        if (confirmed_here_) {
            state_ = State::Paired;
            expecting_ = Expecting::Nothing;
        }
        return {};

    case Expecting::Nothing:
        break;
    }
    return fail(tr("The other computer sent something out of turn."));
}

QByteArray Protocol::confirm()
{
    if (state_ != State::Comparing || confirmed_here_)
        return {};
    confirmed_here_ = true;
    if (confirmed_there_) {
        state_ = State::Paired;
        expecting_ = Expecting::Nothing;
    }
    return line({{"confirm", true}});
}

void Protocol::abandon(const QString& error)
{
    if (state_ != State::Paired)
        fail(error);
}

QList<QByteArray> Protocol::fail(const QString& error, const QString& reason)
{
    // the first reason is the one that matters
    if (state_ == State::Failed)
        return {};
    state_ = State::Failed;
    expecting_ = Expecting::Nothing;
    error_ = error;
    if (reason.isEmpty())
        return {};
    return {line({{"error", reason}})};
}

bool Protocol::take_hello(const QJsonObject& message, QList<QByteArray>& refusal)
{
    if (!message.contains("pairing")) {
        refusal = fail(tr("The other computer sent something that isn't Leapdesk KVM pairing."));
        return false;
    }
    if (message.value("pairing").toInt(0) != kVersion) {
        refusal = fail(other_version(), "version");
        return false;
    }

    const QString name = message.value("name").toString();
    const QByteArray fingerprint = bytes_from_hex(message.value("fingerprint"));
    if (!is_screen_name(name) || fingerprint.isEmpty()) {
        refusal = fail(tr("The other computer sent something that isn't Leapdesk KVM pairing."));
        return false;
    }
    if (fingerprint == fingerprint_) {
        refusal = fail(this_computer(), "self");
        return false;
    }

    peer_name_ = name;
    peer_fingerprint_ = fingerprint;
    return true;
}

void Protocol::compare()
{
    const bool server = role_ == Role::Server;
    code_ = pairing::code(server ? fingerprint_ : peer_fingerprint_, server ? peer_fingerprint_ : fingerprint_,
                          server ? nonce_ : peer_nonce_, server ? peer_nonce_ : nonce_);
    state_ = State::Comparing;
    expecting_ = Expecting::Confirmation;
}

Connection::Connection(QTcpSocket* socket, const Protocol& protocol, QObject* parent) :
    QObject(parent),
    socket_(socket),
    protocol_(protocol),
    timer_(new QTimer(this)),
    reached_(socket->state() == QAbstractSocket::ConnectedState)
{
    socket_->setParent(this);
    socket_->setReadBufferSize(kMaxLine * 4);

    connect(socket_, &QTcpSocket::readyRead, this, &Connection::read);
    connect(socket_, &QTcpSocket::disconnected, this, [this]() {
        stop(tr("The other computer stopped pairing."));
    });
    connect(socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        stop(reached_ ? tr("The other computer stopped pairing.") : socket_->errorString());
    });

    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this]() {
        stop(reached_ ? tr("The other computer stopped answering.") : tr("It didn't answer."));
    });
    timer_->start(kAnswerMilliseconds);

    if (protocol_.role() == Protocol::Role::Client) {
        auto say_hello = [this]() {
            reached_ = true;
            send({protocol_.hello()});
            settle();
        };
        if (reached_)
            say_hello();
        else
            connect(socket_, &QTcpSocket::connected, this, say_hello);
    }
}

bool Connection::worth_retrying() const
{
    return protocol_.state() == Protocol::State::Failed && (!reached_ || protocol_.peer_busy());
}

void Connection::confirm()
{
    send({protocol_.confirm()});
    settle();
}

void Connection::refuse(QTcpSocket* socket)
{
    // closing with its hello unread would reset the connection, which can lose the answer,
    // so the other computer closes it once it has read that
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() { socket->readAll(); });
    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    QTimer::singleShot(kAnswerMilliseconds, socket, &QObject::deleteLater);
    socket->write(Protocol::busy() + '\n');
}

void Connection::read()
{
    while (socket_->canReadLine() && protocol_.state() != Protocol::State::Failed) {
        const QByteArray line = socket_->readLine(kMaxLine);
        if (!line.endsWith('\n')) {
            stop(tr("The other computer sent something that isn't Leapdesk KVM pairing."));
            return;
        }
        send(protocol_.receive(line.trimmed()));
    }
    if (!socket_->canReadLine() && socket_->bytesAvailable() > kMaxLine) {
        stop(tr("The other computer sent something that isn't Leapdesk KVM pairing."));
        return;
    }
    settle();
}

void Connection::send(const QList<QByteArray>& lines)
{
    for (const QByteArray& line : lines) {
        if (!line.isEmpty())
            socket_->write(line + '\n');
    }
}

void Connection::stop(const QString& error)
{
    if (protocol_.state() == Protocol::State::Failed || protocol_.state() == Protocol::State::Paired)
        return;
    protocol_.abandon(error);
    settle();
}

void Connection::settle()
{
    switch (protocol_.state()) {
    case Protocol::State::Comparing:
        timer_->stop();
        break;
    case Protocol::State::Paired:
        timer_->stop();
        // the window closes as soon as it's paired, so the last confirmation has to be on
        // its way first
        socket_->flush();
        socket_->disconnectFromHost();
        break;
    case Protocol::State::Failed:
        timer_->stop();
        if (socket_->state() == QAbstractSocket::ConnectedState) {
            // with any reason to tell the other computer on its way first
            socket_->flush();
            socket_->disconnectFromHost();
        } else {
            // a connection still being made would otherwise carry on
            socket_->abort();
        }
        break;
    default:
        break;
    }
    Q_EMIT changed();
}

} // namespace pairing
