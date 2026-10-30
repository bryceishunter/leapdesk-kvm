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

#include "../src/Pairing.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>

using pairing::Protocol;
using Role = Protocol::Role;
using State = Protocol::State;

namespace {

QByteArray filled(int byte)
{
    return QByteArray(32, static_cast<char>(byte));
}

const QByteArray kServerFingerprint = filled(0x11);
const QByteArray kClientFingerprint = filled(0x22);
const QByteArray kMiddleFingerprint = filled(0x33);

// hands each line to the other side, and its answers back, until neither has more to say
void deliver(Protocol& to, const QList<QByteArray>& lines, Protocol& back)
{
    for (const QByteArray& line : lines)
        deliver(back, to.receive(line), to);
}

QJsonObject parsed(const QByteArray& line)
{
    return QJsonDocument::fromJson(line).object();
}

QByteArray line(const QJsonObject& message)
{
    return QJsonDocument(message).toJson(QJsonDocument::Compact);
}

QString hex(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toHex());
}

} // namespace

TEST(PairingTests, bothSidesShowTheSameCodeAndLearnEachOther)
{
    Protocol server(Role::Server, "desk", kServerFingerprint);
    Protocol client(Role::Client, "studio", kClientFingerprint);

    deliver(server, {client.hello()}, client);

    ASSERT_EQ(server.state(), State::Comparing) << server.error().toStdString();
    ASSERT_EQ(client.state(), State::Comparing) << client.error().toStdString();
    EXPECT_EQ(server.code(), client.code());
    EXPECT_EQ(server.code().size(), 6);
    EXPECT_EQ(server.peer_name(), "studio");
    EXPECT_EQ(client.peer_name(), "desk");
    EXPECT_EQ(server.peer_fingerprint(), kClientFingerprint);
    EXPECT_EQ(client.peer_fingerprint(), kServerFingerprint);
}

TEST(PairingTests, pairedOnlyOnceBothPeopleConfirm)
{
    Protocol server(Role::Server, "desk", kServerFingerprint);
    Protocol client(Role::Client, "studio", kClientFingerprint);
    deliver(server, {client.hello()}, client);

    deliver(client, {server.confirm()}, server);
    EXPECT_EQ(server.state(), State::Comparing);
    EXPECT_EQ(client.state(), State::Comparing);
    EXPECT_TRUE(client.confirmed_there());

    deliver(server, {client.confirm()}, client);
    EXPECT_EQ(server.state(), State::Paired);
    EXPECT_EQ(client.state(), State::Paired);
}

TEST(PairingTests, eachNewExchangeHasANewCode)
{
    Protocol server(Role::Server, "desk", kServerFingerprint);
    Protocol client(Role::Client, "studio", kClientFingerprint);
    deliver(server, {client.hello()}, client);
    Protocol server_again(Role::Server, "desk", kServerFingerprint);
    Protocol client_again(Role::Client, "studio", kClientFingerprint);
    deliver(server_again, {client_again.hello()}, client_again);

    // one in a million they match by chance
    EXPECT_NE(server.code(), server_again.code());
}

TEST(PairingTests, codeAndCommitmentAreTheAgreedHashes)
{
    // worked out separately, with Python's hashlib, so both computers agree across versions
    EXPECT_EQ(pairing::code(kServerFingerprint, kClientFingerprint, filled(0x01), filled(0x02)), "856122");
    EXPECT_EQ(hex(pairing::commitment(kServerFingerprint, kClientFingerprint, filled(0x01))),
              "37bb75f8c64583796facda6617d1872bf378cf559012d756ddb1a8134df263c4");
}

TEST(PairingTests, someoneInBetweenGetsDifferentCodesOnEachSide)
{
    // they pair with each side using a certificate of their own
    Protocol server(Role::Server, "desk", kServerFingerprint, filled(0x01));
    Protocol client(Role::Client, "studio", kClientFingerprint, filled(0x02));
    Protocol posing_as_server(Role::Server, "desk", kMiddleFingerprint, filled(0x01));
    Protocol posing_as_client(Role::Client, "studio", kMiddleFingerprint, filled(0x02));

    deliver(posing_as_server, {client.hello()}, client);
    deliver(server, {posing_as_client.hello()}, posing_as_client);

    ASSERT_EQ(client.state(), State::Comparing);
    ASSERT_EQ(server.state(), State::Comparing);
    EXPECT_NE(client.code(), server.code());
}

TEST(PairingTests, aPromisePassedOnWithAnotherCertificateIsRefused)
{
    // someone in between shows the client a certificate of their own, and passes on the
    // real server's promise and nonce so as to get the server's code
    Protocol server(Role::Server, "desk", kServerFingerprint);
    Protocol posing_as_client(Role::Client, "studio", kMiddleFingerprint);
    Protocol client(Role::Client, "studio", kClientFingerprint);

    QJsonObject promise = parsed(server.receive(posing_as_client.hello()).value(0));
    client.hello();
    promise["fingerprint"] = hex(kMiddleFingerprint);
    QList<QByteArray> client_nonce = client.receive(line(promise));
    ASSERT_EQ(client_nonce.size(), 1);

    QList<QByteArray> server_nonce = server.receive(client_nonce[0]);
    ASSERT_EQ(server_nonce.size(), 1);
    client.receive(server_nonce[0]);

    EXPECT_EQ(client.state(), State::Failed);
    EXPECT_TRUE(client.code().isEmpty());
}

TEST(PairingTests, aNonceOtherThanThePromisedOneIsRefused)
{
    Protocol server(Role::Server, "desk", kServerFingerprint);
    Protocol client(Role::Client, "studio", kClientFingerprint);

    QList<QByteArray> promise = server.receive(client.hello());
    QList<QByteArray> client_nonce = client.receive(promise.value(0));
    server.receive(client_nonce.value(0));

    client.receive(line({{"nonce", hex(filled(0x44))}}));

    EXPECT_EQ(client.state(), State::Failed);
    EXPECT_TRUE(client.code().isEmpty());
}

TEST(PairingTests, aServerWaitsForTheClientsNonceBeforeShowingItsOwn)
{
    Protocol server(Role::Server, "desk", kServerFingerprint);
    Protocol client(Role::Client, "studio", kClientFingerprint);

    QJsonObject promise = parsed(server.receive(client.hello()).value(0));

    EXPECT_FALSE(promise.contains("nonce"));
    EXPECT_TRUE(promise.contains("commitment"));
    EXPECT_EQ(server.state(), State::Exchanging);
}

TEST(PairingTests, messagesOutOfTurnStopIt)
{
    Protocol server(Role::Server, "desk", kServerFingerprint);
    server.receive(line({{"nonce", hex(filled(0x01))}}));
    EXPECT_EQ(server.state(), State::Failed);

    Protocol client(Role::Client, "studio", kClientFingerprint);
    client.hello();
    client.receive(line({{"confirm", true}}));
    EXPECT_EQ(client.state(), State::Failed);

    Protocol early(Role::Client, "studio", kClientFingerprint);
    EXPECT_TRUE(early.confirm().isEmpty());
    EXPECT_FALSE(early.confirmed_here());
}

TEST(PairingTests, anythingButPairingStopsIt)
{
    const QList<QByteArray> hellos = {
        "not json",
        "[1, 2]",
        line({{"name", "studio"}, {"fingerprint", hex(kClientFingerprint)}}),
        line({{"pairing", 1}, {"name", "two words"}, {"fingerprint", hex(kClientFingerprint)}}),
        line({{"pairing", 1}, {"name", "studio\n"}, {"fingerprint", hex(kClientFingerprint)}}),
        line({{"pairing", 1}, {"name", ""}, {"fingerprint", hex(kClientFingerprint)}}),
        line({{"pairing", 1}, {"name", QString(300, 'a')}, {"fingerprint", hex(kClientFingerprint)}}),
        line({{"pairing", 1}, {"name", "studio"}, {"fingerprint", hex(kClientFingerprint).left(62)}}),
        line({{"pairing", 1}, {"name", "studio"}, {"fingerprint", QString(64, 'g')}}),
        line({{"pairing", 1}, {"name", "studio"}, {"fingerprint", 7}}),
    };
    for (const QByteArray& hello : hellos) {
        Protocol server(Role::Server, "desk", kServerFingerprint);
        EXPECT_TRUE(server.receive(hello).isEmpty()) << hello.toStdString();
        EXPECT_EQ(server.state(), State::Failed) << hello.toStdString();
        EXPECT_TRUE(server.peer_name().isEmpty());
    }
}

TEST(PairingTests, anotherVersionSaysSo)
{
    Protocol server(Role::Server, "desk", kServerFingerprint);
    server.receive(line({{"pairing", 2}, {"name", "studio"}, {"fingerprint", hex(kClientFingerprint)}}));

    EXPECT_EQ(server.state(), State::Failed);
    EXPECT_TRUE(server.error().contains("version"));
}

TEST(PairingTests, aComputerCantPairWithItself)
{
    Protocol server(Role::Server, "desk", kServerFingerprint);
    Protocol client(Role::Client, "desk", kServerFingerprint);

    deliver(server, {client.hello()}, client);

    EXPECT_EQ(server.state(), State::Failed);
    EXPECT_EQ(client.state(), State::Failed);
    // the server tells the client why
    EXPECT_EQ(client.error(), server.error());
}

TEST(PairingTests, aBusyServerIsWorthTryingAgain)
{
    Protocol client(Role::Client, "studio", kClientFingerprint);
    client.hello();

    client.receive(Protocol::busy());

    EXPECT_EQ(client.state(), State::Failed);
    EXPECT_TRUE(client.peer_busy());
}

TEST(PairingTests, abandoningKeepsTheFirstReasonAndLeavesAPairingAlone)
{
    Protocol server(Role::Server, "desk", kServerFingerprint);
    server.abandon("first");
    server.abandon("second");
    EXPECT_EQ(server.error(), "first");

    Protocol paired_server(Role::Server, "desk", kServerFingerprint);
    Protocol paired_client(Role::Client, "studio", kClientFingerprint);
    deliver(paired_server, {paired_client.hello()}, paired_client);
    deliver(paired_client, {paired_server.confirm()}, paired_server);
    deliver(paired_server, {paired_client.confirm()}, paired_client);
    paired_server.abandon("closed");
    EXPECT_EQ(paired_server.state(), State::Paired);
}

TEST(PairingTests, nonceIsRandom)
{
    QByteArray first = pairing::new_nonce();
    EXPECT_EQ(first.size(), 32);
    EXPECT_NE(first, pairing::new_nonce());
}

TEST(PairingTests, hostOfAServerAddress)
{
    EXPECT_EQ(pairing::host_of("100.111.152.25:24800"), "100.111.152.25");
    EXPECT_EQ(pairing::host_of(" desktop-gp522k1 "), "desktop-gp522k1");
    EXPECT_EQ(pairing::host_of("desk:24800"), "desk");
    EXPECT_EQ(pairing::host_of("[fd7a:115c::1]:24800"), "fd7a:115c::1");
    EXPECT_EQ(pairing::host_of("fd7a:115c::1"), "fd7a:115c::1");
    EXPECT_EQ(pairing::spaced("482913"), "482 913");
}

namespace {

class PairingConnectionTests : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (QCoreApplication::instance() == nullptr) {
            static int argc = 1;
            static char name[] = "guiunittests";
            static char* argv[] = {name, nullptr};
            static QCoreApplication app(argc, argv);
        }
    }

    // runs the event loop until done, or for a few seconds
    static bool wait_for(const std::function<bool()>& done)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!done() && elapsed.elapsed() < 5000) {
            QEventLoop loop;
            QTimer::singleShot(10, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return done();
    }

    void SetUp() override
    {
        ASSERT_TRUE(listener_.listen(QHostAddress::LocalHost, 0));
    }

    // a client connection to the listener
    std::unique_ptr<pairing::Connection> connect_client(const QByteArray& fingerprint = kClientFingerprint)
    {
        auto* socket = new QTcpSocket;
        auto client = std::make_unique<pairing::Connection>(
            socket, Protocol(Role::Client, "studio", fingerprint));
        socket->connectToHost(QHostAddress::LocalHost, listener_.serverPort());
        return client;
    }

    QTcpServer listener_;
};

} // namespace

TEST_F(PairingConnectionTests, pairsOverTheNetwork)
{
    std::unique_ptr<pairing::Connection> server;
    QObject::connect(&listener_, &QTcpServer::newConnection, [&]() {
        server = std::make_unique<pairing::Connection>(
            listener_.nextPendingConnection(), Protocol(Role::Server, "desk", kServerFingerprint));
    });
    auto client = connect_client();

    ASSERT_TRUE(wait_for([&]() {
        return server && server->protocol().state() == State::Comparing &&
               client->protocol().state() == State::Comparing;
    }));
    EXPECT_EQ(server->protocol().code(), client->protocol().code());

    client->confirm();
    server->confirm();

    ASSERT_TRUE(wait_for([&]() {
        return server->protocol().state() == State::Paired && client->protocol().state() == State::Paired;
    }));
    EXPECT_EQ(server->protocol().peer_fingerprint(), kClientFingerprint);
    EXPECT_EQ(client->protocol().peer_fingerprint(), kServerFingerprint);
}

TEST_F(PairingConnectionTests, theLastConfirmationArrivesThoughItsWindowClosesAtOnce)
{
    std::unique_ptr<pairing::Connection> server;
    QObject::connect(&listener_, &QTcpServer::newConnection, [&]() {
        server = std::make_unique<pairing::Connection>(
            listener_.nextPendingConnection(), Protocol(Role::Server, "desk", kServerFingerprint));
    });
    auto client = connect_client();
    ASSERT_TRUE(wait_for([&]() { return server && server->protocol().state() == State::Comparing &&
                                        client->protocol().state() == State::Comparing; }));
    client->confirm();
    ASSERT_TRUE(wait_for([&]() { return server->protocol().confirmed_there(); }));

    // as the dialog does once it's paired
    server->confirm();
    ASSERT_EQ(server->protocol().state(), State::Paired);
    server.reset();

    ASSERT_TRUE(wait_for([&]() { return client->protocol().state() != State::Comparing; }));
    EXPECT_EQ(client->protocol().state(), State::Paired) << client->protocol().error().toStdString();
}

TEST_F(PairingConnectionTests, theServerCanWaitAgainRightAfterPairing)
{
    const quint16 port = listener_.serverPort();
    std::unique_ptr<pairing::Connection> server;
    QObject::connect(&listener_, &QTcpServer::newConnection, [&]() {
        server = std::make_unique<pairing::Connection>(
            listener_.nextPendingConnection(), Protocol(Role::Server, "desk", kServerFingerprint));
    });
    auto client = connect_client();
    ASSERT_TRUE(wait_for([&]() { return server && server->protocol().state() == State::Comparing &&
                                        client->protocol().state() == State::Comparing; }));
    client->confirm();
    server->confirm();
    ASSERT_TRUE(wait_for([&]() { return client->protocol().state() == State::Paired; }));
    server.reset();
    client.reset();
    listener_.close();

    EXPECT_TRUE(listener_.listen(QHostAddress::LocalHost, port)) << listener_.errorString().toStdString();
}

TEST_F(PairingConnectionTests, aServerAlreadyPairingTurnsAnotherAway)
{
    QObject::connect(&listener_, &QTcpServer::newConnection, [&]() {
        pairing::Connection::refuse(listener_.nextPendingConnection());
    });
    auto client = connect_client();

    ASSERT_TRUE(wait_for([&]() { return client->protocol().state() == State::Failed; }));
    EXPECT_TRUE(client->protocol().peer_busy());
    EXPECT_TRUE(client->worth_retrying());
}

TEST_F(PairingConnectionTests, aServerNotWaitingIsWorthTryingAgain)
{
    const quint16 port = listener_.serverPort();
    listener_.close();

    auto* socket = new QTcpSocket;
    pairing::Connection client(socket, Protocol(Role::Client, "studio", kClientFingerprint));
    socket->connectToHost(QHostAddress::LocalHost, port);

    ASSERT_TRUE(wait_for([&]() { return client.protocol().state() == State::Failed; }));
    EXPECT_TRUE(client.worth_retrying());
}

TEST_F(PairingConnectionTests, cancellingOnOneSideStopsTheOther)
{
    std::unique_ptr<pairing::Connection> server;
    QObject::connect(&listener_, &QTcpServer::newConnection, [&]() {
        server = std::make_unique<pairing::Connection>(
            listener_.nextPendingConnection(), Protocol(Role::Server, "desk", kServerFingerprint));
    });
    auto client = connect_client();
    ASSERT_TRUE(wait_for([&]() { return client->protocol().state() == State::Comparing; }));

    client->confirm();
    server.reset();

    ASSERT_TRUE(wait_for([&]() { return client->protocol().state() == State::Failed; }));
    EXPECT_FALSE(client->worth_retrying());
}

TEST_F(PairingConnectionTests, aFloodWithoutLinesStopsIt)
{
    QTcpSocket* flooder = nullptr;
    std::unique_ptr<pairing::Connection> server;
    QObject::connect(&listener_, &QTcpServer::newConnection, [&]() {
        server = std::make_unique<pairing::Connection>(
            listener_.nextPendingConnection(), Protocol(Role::Server, "desk", kServerFingerprint));
    });
    flooder = new QTcpSocket(&listener_);
    flooder->connectToHost(QHostAddress::LocalHost, listener_.serverPort());
    ASSERT_TRUE(flooder->waitForConnected(5000));
    flooder->write(QByteArray(64 * 1024, 'x'));

    ASSERT_TRUE(wait_for([&]() { return server && server->protocol().state() == State::Failed; }));
}
