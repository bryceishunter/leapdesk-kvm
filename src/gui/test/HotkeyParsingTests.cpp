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

#include "../src/Hotkey.h"
#include <gtest/gtest.h>

#include <QtCore/QTextStream>

namespace {

QString line(const Hotkey& hotkey)
{
    QString text;
    QTextStream stream(&text);
    stream << hotkey;
    stream.flush();
    return text.trimmed();
}

} // namespace

TEST(KeySequenceParsingTests, fromString_modifiersAndKey_roundTrips)
{
    KeySequence sequence;
    ASSERT_TRUE(KeySequence::fromString("Control+Alt+F1", false, sequence));
    EXPECT_EQ(sequence.toString(), "Control+Alt+F1");
    EXPECT_TRUE(sequence.valid());
}

TEST(KeySequenceParsingTests, fromString_letter_isWrittenLowercase)
{
    KeySequence sequence;
    ASSERT_TRUE(KeySequence::fromString("Shift+a", false, sequence));
    EXPECT_EQ(sequence.toString(), "Shift+a");
}

TEST(KeySequenceParsingTests, fromString_mouseButton_roundTrips)
{
    KeySequence sequence;
    ASSERT_TRUE(KeySequence::fromString("Control+2", true, sequence));
    EXPECT_TRUE(sequence.isMouseButton());
    EXPECT_EQ(sequence.toString(), "Control+2");
}

TEST(KeySequenceParsingTests, fromString_unsupportedModifier_fails)
{
    KeySequence sequence;
    EXPECT_FALSE(KeySequence::fromString("Super+a", false, sequence));
    EXPECT_FALSE(KeySequence::fromString("AltGr+a", false, sequence));
}

TEST(KeySequenceParsingTests, fromString_unknownKey_fails)
{
    KeySequence sequence;
    EXPECT_FALSE(KeySequence::fromString("Control+NoSuchKey", false, sequence));
    EXPECT_FALSE(KeySequence::fromString("", false, sequence));
}

TEST(HotkeyParsingTests, fromText_pressAndReleaseActions_roundTrip)
{
    Hotkey hotkey;
    ASSERT_TRUE(Hotkey::fromText(
        "keystroke(Control+Shift+a) = keystroke(Alt+F4,*), switchToScreen(desk); "
        "lockCursorToScreen(off)", hotkey));

    ASSERT_EQ(hotkey.actions().size(), 3u);
    EXPECT_EQ(hotkey.actions()[0].type(), Action::keystroke);
    EXPECT_FALSE(hotkey.actions()[0].haveScreens());
    EXPECT_EQ(hotkey.actions()[1].switchScreenName(), "desk");
    EXPECT_TRUE(hotkey.actions()[2].activeOnRelease());
    EXPECT_EQ(hotkey.actions()[2].lockCursorMode(), Action::lockCursorOff);

    EXPECT_EQ(line(hotkey), "keystroke(Control+Shift+a) = keystroke(Alt+F4,*), "
                            "switchToScreen(desk), ;lockCursorToScreen(off)");
}

TEST(HotkeyParsingTests, fromText_keystrokeOnScreens_keepsScreenList)
{
    Hotkey hotkey;
    ASSERT_TRUE(Hotkey::fromText("keystroke(F5) = keyDown(Control+c,desk:studio)", hotkey));

    ASSERT_EQ(hotkey.actions().size(), 1u);
    EXPECT_EQ(hotkey.actions()[0].type(), Action::keyDown);
    EXPECT_TRUE(hotkey.actions()[0].haveScreens());
    EXPECT_EQ(hotkey.actions()[0].typeScreenNames(), QStringList({"desk", "studio"}));
}

TEST(HotkeyParsingTests, fromText_toggleAndDirection_parse)
{
    Hotkey hotkey;
    ASSERT_TRUE(Hotkey::fromText("mousebutton(Alt+3) = toggleScreen, switchInDirection(up)", hotkey));

    EXPECT_TRUE(hotkey.keySequence().isMouseButton());
    ASSERT_EQ(hotkey.actions().size(), 2u);
    EXPECT_EQ(hotkey.actions()[0].type(), Action::toggleScreen);
    EXPECT_EQ(hotkey.actions()[1].switchDirection(), Action::switchUp);
}

TEST(HotkeyParsingTests, fromText_unsupportedAction_fails)
{
    Hotkey hotkey;
    EXPECT_FALSE(Hotkey::fromText("keystroke(F12) = keyboardBroadcast(toggle)", hotkey));
    EXPECT_FALSE(Hotkey::fromText("connected(desk) = switchToScreen(desk)", hotkey));
    EXPECT_FALSE(Hotkey::fromText("keystroke(F12)", hotkey));
}

TEST(HotkeyParsingTests, actionsSayWhatTheyDo)
{
    Hotkey hotkey;
    ASSERT_TRUE(Hotkey::fromText("keystroke(Control+Alt+Down) = switchToScreen(desk), toggleScreen, "
                                 "switchInDirection(up), lockCursorToScreen(on), "
                                 "keystroke(Control+c,studio); keyDown(F5)", hotkey));
    ASSERT_EQ(hotkey.actions().size(), 6u);

    EXPECT_EQ(hotkey.actions()[0].description(), "Switch to desk");
    EXPECT_EQ(hotkey.actions()[1].description(), "Switch to the next screen");
    EXPECT_EQ(hotkey.actions()[2].description(), "Switch to the screen above");
    EXPECT_EQ(hotkey.actions()[3].description(), "Lock the pointer to its screen");
    EXPECT_EQ(hotkey.actions()[4].description(), "Press and release Control+c on studio");
    EXPECT_EQ(hotkey.actions()[5].description(), "Press F5, when released");
}
