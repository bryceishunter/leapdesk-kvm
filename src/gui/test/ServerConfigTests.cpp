/*
 * Leapdesk KVM -- mouse and keyboard sharing utility
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

#include "../src/ServerConfig.h"
#include "server/Config.h"

#include <gtest/gtest.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>

#include <sstream>

namespace {

// Two monitors side by side form one 4480px wide screen; only the right
// monitor's bottom edge (from 1920px on) leads down to the studio.
const char kPartialEdgeLayout[] =
    "# Server layout.\n"
    "# After editing, reload the configuration.\n"
    "\n"
    "section: screens\n"
    "    desk:\n"
    "    studio:\n"
    "end\n"
    "\n"
    "section: links\n"
    "    # a comment inside a section, which a save drops\n"
    "    desk:\n"
    "        down(42.86,100) = studio\n"
    "    studio:\n"
    "        up = desk(42.86,100)\n"
    "end\n"
    "\n"
    "section: options\n"
    "    clipboardSharing = true\n"
    "end\n";

// Exposes the setters the settings dialog uses.
class TestServerConfig : public ServerConfig
{
public:
    using ServerConfig::ServerConfig;
    using ServerConfig::screens;
    using ServerConfig::links;
    using ServerConfig::hotkeys;
    using ServerConfig::setClipboardSharing;
    using ServerConfig::renameScreen;
    using ServerConfig::removeScreen;
};

class ServerConfigTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir_.isValid());
        settings_ = std::make_unique<QSettings>(dir_.filePath("settings.ini"), QSettings::IniFormat);
    }

    QString writeFile(const QString& name, const QString& text)
    {
        QString path = dir_.filePath(name);
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(text.toUtf8());
        return path;
    }

    static QString readFile(const QString& path)
    {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
        return QString::fromUtf8(file.readAll());
    }

    // what the server reads from a file
    static inputleap::Config serverView(const QString& text)
    {
        inputleap::Config config;
        std::istringstream in(text.toStdString());
        in >> config;
        return config;
    }

    TestServerConfig loaded(const QString& text)
    {
        TestServerConfig config(settings_.get(), "desk", nullptr);
        QString error;
        EXPECT_TRUE(config.load(writeFile("server.sgc", text), &error)) << error.toStdString();
        return config;
    }

    QString saved(TestServerConfig& config)
    {
        QString path = dir_.filePath("server.sgc");
        QString error;
        EXPECT_TRUE(config.save(path, &error)) << error.toStdString();
        return readFile(path);
    }

    QTemporaryDir dir_;
    std::unique_ptr<QSettings> settings_;
};

} // namespace

TEST_F(ServerConfigTests, load_partialEdgeLinks_keepsTheirStretches)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);

    ASSERT_EQ(config.links().size(), 2u);
    const ScreenLink& down = config.links()[0];
    EXPECT_EQ(down.source, "desk");
    EXPECT_EQ(down.side, ScreenLink::Side::Down);
    EXPECT_DOUBLE_EQ(down.sourceStart, 42.86);
    EXPECT_DOUBLE_EQ(down.sourceEnd, 100);
    EXPECT_EQ(down.target, "studio");
    EXPECT_TRUE(down.coversWholeTargetEdge());
}

TEST_F(ServerConfigTests, save_unchanged_leavesTheFileAsItIs)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);

    QString text = saved(config);

    EXPECT_EQ(text, QString(kPartialEdgeLayout));
    EXPECT_FALSE(QFile::exists(dir_.filePath("server.sgc.bak")));
}

TEST_F(ServerConfigTests, save_edited_serverReadsTheSameLinks)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    config.setClipboardSharing(false);

    QString text = saved(config);

    EXPECT_TRUE(text.contains("down(42.86,100) = studio")) << text.toStdString();
    EXPECT_TRUE(text.contains("up = desk(42.86,100)")) << text.toStdString();
    EXPECT_TRUE(QFile::exists(dir_.filePath("server.sgc.bak")))
        << QDir(dir_.path()).entryList(QDir::Files).join(", ").toStdString();
}

TEST_F(ServerConfigTests, save_edited_keepsLeadingComments)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    config.setClipboardSharing(false);

    QString text = saved(config);

    EXPECT_TRUE(text.startsWith("# Server layout.\n# After editing, reload the configuration.\n"))
        << text.toStdString();
}

TEST_F(ServerConfigTests, save_edited_addsNoDefaultOptions)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    config.setClipboardSharing(false);

    QString text = saved(config);

    EXPECT_FALSE(text.contains("relativeMouseMoves")) << text.toStdString();
    EXPECT_FALSE(text.contains("halfDuplexCapsLock")) << text.toStdString();
    EXPECT_FALSE(text.contains("switchCorners")) << text.toStdString();
}

TEST_F(ServerConfigTests, save_changedOption_changesOnlyThatOption)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    config.setClipboardSharing(false);

    QString text = saved(config);

    EXPECT_TRUE(text.contains("clipboardSharing = false")) << text.toStdString();
    inputleap::Config expected = serverView(kPartialEdgeLayout);
    expected.addOption("", kOptionClipboardSharing, 0);
    EXPECT_EQ(serverView(text), expected) << text.toStdString();
}

TEST_F(ServerConfigTests, save_renamedScreen_renamesItsLinks)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    config.renameScreen("studio", "surface");

    QString text = saved(config);

    EXPECT_TRUE(text.contains("down(42.86,100) = surface")) << text.toStdString();
    EXPECT_TRUE(text.contains("surface:\n\t\tup = desk(42.86,100)")) << text.toStdString();
    EXPECT_FALSE(text.contains("studio")) << text.toStdString();
}

TEST_F(ServerConfigTests, save_editedStretch_writesTheNewStretch)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    config.links()[0].sourceStart = 50;
    config.links()[1].targetStart = 50;

    QString text = saved(config);

    EXPECT_TRUE(text.contains("down(50,100) = studio")) << text.toStdString();
    EXPECT_TRUE(text.contains("up = desk(50,100)")) << text.toStdString();
}

TEST_F(ServerConfigTests, save_removedScreen_dropsItAndItsLinks)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    config.removeScreen("studio");

    QString text = saved(config);

    EXPECT_FALSE(text.contains("studio")) << text.toStdString();
    EXPECT_TRUE(text.contains("desk:")) << text.toStdString();
}

TEST_F(ServerConfigTests, save_editedScreen_keepsSettingsTheEditorDoesNotShow)
{
    const QString layout =
        "section: screens\n"
        "    desk:\n"
        "        altgr = ctrl\n"
        "    studio:\n"
        "end\n"
        "section: options\n"
        "    switchNeedsShift = true\n"
        "end\n";
    TestServerConfig config = loaded(layout);
    config.renameScreen("studio", "surface");
    config.setClipboardSharing(false);

    QString text = saved(config);

    EXPECT_TRUE(text.contains("altgr = ctrl")) << text.toStdString();
    EXPECT_TRUE(text.contains("switchNeedsShift = true")) << text.toStdString();
}

TEST_F(ServerConfigTests, load_hotkeys_splitsEditableFromPreserved)
{
    const QString layout =
        "section: screens\n"
        "    desk:\n"
        "    studio:\n"
        "end\n"
        "section: options\n"
        "    keystroke(Control+Alt+Right) = switchInDirection(right)\n"
        "    keystroke(Control+F12) = keyboardBroadcast(toggle)\n"
        "end\n";
    TestServerConfig config = loaded(layout);

    ASSERT_EQ(config.hotkeys().size(), 1u);
    EXPECT_EQ(config.hotkeys()[0].text(), "keystroke(Control+Alt+Right)");
    ASSERT_EQ(config.preservedRules().size(), 1);
    EXPECT_TRUE(config.preservedRules()[0].startsWith("keystroke(Control+F12)"));

    QString text = saved(config);
    EXPECT_EQ(serverView(text), serverView(layout)) << text.toStdString();
}

TEST_F(ServerConfigTests, save_removedHotkey_keepsPreservedRules)
{
    const QString layout =
        "section: screens\n"
        "    desk:\n"
        "end\n"
        "section: options\n"
        "    keystroke(Control+Alt+Right) = switchInDirection(right)\n"
        "    keystroke(Control+F12) = keyboardBroadcast(toggle)\n"
        "end\n";
    TestServerConfig config = loaded(layout);
    config.hotkeys().clear();

    QString text = saved(config);

    EXPECT_FALSE(text.contains("switchInDirection")) << text.toStdString();
    EXPECT_TRUE(text.contains("keyboardBroadcast(toggle)")) << text.toStdString();
}

TEST_F(ServerConfigTests, load_syntaxError_reportsTheLineAndChangesNothing)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    QString path = writeFile("broken.sgc", "section: screens\n    desk:\nend\nsection: links\n    desk:\n        sideways = desk\nend\n");

    QString error;
    EXPECT_FALSE(config.load(path, &error));

    EXPECT_TRUE(error.contains("line 6: unknown side \"sideways\"")) << error.toStdString();
    EXPECT_EQ(config.links().size(), 2u);
}

TEST_F(ServerConfigTests, changedOnDisk_afterHandEdit_isTrue)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    QString path = dir_.filePath("server.sgc");
    EXPECT_FALSE(config.changedOnDisk(path));

    writeFile("server.sgc", QString(kPartialEdgeLayout).replace("42.86", "40"));

    EXPECT_TRUE(config.changedOnDisk(path));
}

TEST_F(ServerConfigTests, ensureServerScreen_renamedServer_keepsItsLinks)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    config.setServerName("newdesk");

    config.ensureServerScreen("desk");

    inputleap::Config server = serverView(saved(config));
    EXPECT_TRUE(server.isScreen("newdesk"));
    EXPECT_FALSE(server.isScreen("desk"));
    EXPECT_TRUE(server.hasNeighbor("newdesk", inputleap::kBottom));
    EXPECT_TRUE(server.hasNeighbor("studio", inputleap::kTop));
}

TEST_F(ServerConfigTests, ensureServerScreen_unknownServer_addsItWithoutTouchingTheRest)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    config.setServerName("other");

    config.ensureServerScreen("");

    inputleap::Config server = serverView(saved(config));
    EXPECT_TRUE(server.isScreen("other"));
    EXPECT_TRUE(server.isScreen("desk"));
    EXPECT_TRUE(server.hasNeighbor("desk", inputleap::kBottom));
}

TEST_F(ServerConfigTests, ensureServerScreen_serverAlreadyThere_changesNothing)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);
    config.setServerName("DESK");

    config.ensureServerScreen("studio");

    EXPECT_EQ(config.screens().size(), 2u);
    EXPECT_EQ(saved(config).toStdString(), std::string(kPartialEdgeLayout));
}

TEST_F(ServerConfigTests, addScreenBeside_freeEdge_linksBothWays)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);

    EXPECT_TRUE(config.addScreenBeside("laptop", ScreenLink::Side::Right));

    inputleap::Config server = serverView(saved(config));
    EXPECT_TRUE(server.isScreen("laptop"));
    EXPECT_EQ(server.getNeighbor("desk", inputleap::kRight, 0.5f, nullptr), "laptop");
    EXPECT_EQ(server.getNeighbor("laptop", inputleap::kLeft, 0.5f, nullptr), "desk");
    EXPECT_TRUE(server.hasNeighbor("desk", inputleap::kBottom));
}

TEST_F(ServerConfigTests, addScreenBeside_edgeInUse_addsTheScreenWithoutLinks)
{
    TestServerConfig config = loaded(kPartialEdgeLayout);

    EXPECT_FALSE(config.addScreenBeside("laptop", ScreenLink::Side::Down));

    inputleap::Config server = serverView(saved(config));
    EXPECT_TRUE(server.isScreen("laptop"));
    EXPECT_FALSE(server.hasNeighbor("laptop", inputleap::kTop));
    EXPECT_EQ(server.getNeighbor("desk", inputleap::kBottom, 0.9f, nullptr), "studio");
    EXPECT_TRUE(config.hasScreen("LAPTOP"));
}

TEST_F(ServerConfigTests, save_newConfiguration_writesOnlyWhatWasSet)
{
    TestServerConfig config(settings_.get(), "desk", nullptr);
    config.screens().push_back(Screen("studio"));
    ScreenLink link;
    link.source = "desk";
    link.side = ScreenLink::Side::Right;
    link.target = "studio";
    config.links().push_back(link);
    config.links().push_back(link.reversed());

    QString text = saved(config);

    inputleap::Config server = serverView(text);
    EXPECT_TRUE(server.isScreen("desk"));
    EXPECT_TRUE(server.isScreen("studio"));
    EXPECT_TRUE(server.hasNeighbor("desk", inputleap::kRight));
    EXPECT_TRUE(server.hasNeighbor("studio", inputleap::kLeft));
    EXPECT_FALSE(text.contains("clipboardSharing")) << text.toStdString();
}
