/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2023-2024 InputLeap Developers
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

#include "Action.h"

#include <QSettings>
#include <QTextStream>

const char* Action::action_type_names_[] =
{
    "keyDown", "keyUp", "keystroke",
    "switchToScreen", "toggleScreen",
    "switchInDirection", "lockCursorToScreen",
    "mouseDown", "mouseUp", "mousebutton"
};

const char* Action::switch_direction_names_[] = { "left", "right", "up", "down" };
const char* Action::lock_cursor_mode_names_[] = { "toggle", "on", "off" };
const QString Action::command_template_ = QStringLiteral("(%1)");

Action::Action() :
    key_sequence_(),
    type_(keystroke),
    type_screen_names_(),
    switch_screen_name_(),
    switch_direction_(switchLeft),
    lock_cursor_mode_(lockCursorToggle),
    active_on_release_(false),
    has_screens_(false)
{
}

QString Action::description() const
{
    const QString keys = key_sequence_.toString();
    QString words;
    switch (type()) {
    case keystroke: words = tr("Press and release %1").arg(keys); break;
    case keyDown: words = tr("Press %1").arg(keys); break;
    case keyUp: words = tr("Release %1").arg(keys); break;
    case switchToScreen: words = tr("Switch to %1").arg(switchScreenName()); break;
    case toggleScreen: words = tr("Switch to the next screen"); break;
    case switchInDirection: {
        const char* directions[] = {QT_TRANSLATE_NOOP("Action", "Switch to the screen on the left"),
                                    QT_TRANSLATE_NOOP("Action", "Switch to the screen on the right"),
                                    QT_TRANSLATE_NOOP("Action", "Switch to the screen above"),
                                    QT_TRANSLATE_NOOP("Action", "Switch to the screen below")};
        words = tr(directions[switch_direction_ >= 0 && switch_direction_ < 4 ? switch_direction_ : 0]);
        break;
    }
    case lockCursorToScreen:
        words = lock_cursor_mode_ == lockCursonOn ? tr("Lock the pointer to its screen")
              : lock_cursor_mode_ == lockCursorOff ? tr("Unlock the pointer from its screen")
              : tr("Lock or unlock the pointer on its screen");
        break;
    default: words = text(); break;
    }

    const bool keys_somewhere = type() == keystroke || type() == keyDown || type() == keyUp;
    if (keys_somewhere && !key_sequence_.isMouseButton() && haveScreens() && !typeScreenNames().isEmpty())
        words = tr("%1 on %2").arg(words, typeScreenNames().join(QStringLiteral(", ")));
    if (activeOnRelease())
        words = tr("%1, when released").arg(words);
    return words;
}

QString Action::text() const
{
    /* This function is used to save to config file which is for Leapdesk KVM server to
     * read. However the server config parse does not support functions with ()
     * in the end but now argument inside. If you need a function with no
     * argument, it can not have () in the end.
     */
    QString text = QString(action_type_names_[key_sequence_.isMouseButton() ?
                                             type() + int(mouseDown) : type()]);

    switch (type())
    {
        case keyDown:
        case keyUp:
        case keystroke:
            {
                QString commandArgs = key_sequence_.toString();
                if (!key_sequence_.isMouseButton())
                {
                    const QStringList& screens = typeScreenNames();
                    if (haveScreens() && !screens.isEmpty())
                    {
                        QString screenList;
                        for (int i = 0; i < screens.size(); i++)
                        {
                            screenList.append(screens[i]);
                            if (i != screens.size() - 1)
                                screenList.append(QStringLiteral(":"));
                        }
                        commandArgs.append(QStringLiteral(",%1").arg(screenList));
                    }
                    else
                    {
                        commandArgs.append(QStringLiteral(",*"));
                    }
                }
                text.append(command_template_.arg(commandArgs));
            }
            break;

        case switchToScreen:
            text.append(command_template_.arg(switchScreenName()));
            break;

        case switchInDirection:
            text.append(command_template_.arg(switch_direction_names_[switch_direction_]));
            break;

        case lockCursorToScreen:
            text.append(command_template_.arg(lock_cursor_mode_names_[lock_cursor_mode_]));
            break;

        default:
            break;
    }


    return text;
}

void Action::loadSettings(QSettings& settings)
{
    key_sequence_.loadSettings(settings);
    setType(settings.value("type", keyDown).toInt());

    type_screen_names_.clear();
    int numTypeScreens = settings.beginReadArray("typeScreenNames");
    for (int i = 0; i < numTypeScreens; i++)
    {
        settings.setArrayIndex(i);
        type_screen_names_.append(settings.value(SettingsKeys::SCREEN_NAME).toString());
    }
    settings.endArray();

    setSwitchScreenName(settings.value(SettingsKeys::SWITCH_TO_SCREEN).toString());
    setSwitchDirection(settings.value(SettingsKeys::SWITCH_DIRECTION, switchLeft).toInt());
    setLockCursorMode(settings.value(SettingsKeys::LOCKTOSCREEN, lockCursorToggle).toInt());
    setActiveOnRelease(settings.value(SettingsKeys::ACTIVEONRELEASE, false).toBool());
    setHaveScreens(settings.value(SettingsKeys::HASSCREENS, false).toBool());
}

void Action::saveSettings(QSettings& settings) const
{
    key_sequence_.saveSettings(settings);
    settings.setValue(SettingsKeys::ACTION_TYPE, type());

    settings.beginWriteArray(SettingsKeys::SCREEN_NAMES);
    for (int i = 0; i < typeScreenNames().size(); i++)
    {
        settings.setArrayIndex(i);
        settings.setValue(SettingsKeys::SCREEN_NAME, typeScreenNames()[i]);
    }
    settings.endArray();

    settings.setValue(SettingsKeys::SWITCH_TO_SCREEN, switchScreenName());
    settings.setValue(SettingsKeys::SWITCH_DIRECTION, switchDirection());
    settings.setValue(SettingsKeys::LOCKTOSCREEN, lockCursorMode());
    settings.setValue(SettingsKeys::ACTIVEONRELEASE, activeOnRelease());
    settings.setValue(SettingsKeys::HASSCREENS, haveScreens());
}

QTextStream& operator<<(QTextStream& outStream, const Action& action)
{
    if (action.activeOnRelease())
        outStream << ";";

    outStream << action.text();

    return outStream;
}

static int indexOfName(const char* const names[], int count, const QString& name)
{
    for (int i = 0; i < count; i++) {
        if (name == QLatin1String(names[i]))
            return i;
    }
    return -1;
}

bool Action::fromText(const QString& text, bool activeOnRelease, Action& action)
{
    QString name = text.trimmed();
    QString args;
    bool hasArgs = false;
    int open = name.indexOf(QLatin1Char('('));
    if (open >= 0) {
        if (!name.endsWith(QLatin1Char(')')))
            return false;
        args = name.mid(open + 1, name.size() - open - 2).trimmed();
        name = name.left(open).trimmed();
        hasArgs = true;
    }

    int type = indexOfName(action_type_names_, mousebutton + 1, name);
    if (type < 0)
        return false;

    Action result;
    result.setActiveOnRelease(activeOnRelease);

    switch (type) {
        case keyDown:
        case keyUp:
        case keystroke: {
            QStringList parts = args.split(QLatin1Char(','));
            if (parts.size() > 2)
                return false;
            KeySequence sequence;
            if (!KeySequence::fromString(parts[0].trimmed(), false, sequence))
                return false;
            result.setType(type);
            result.setKeySequence(sequence);
            if (parts.size() == 2 && parts[1].trimmed() != QLatin1String("*")) {
                result.setHaveScreens(true);
                for (const QString& screen : parts[1].split(QLatin1Char(':'))) {
                    if (screen.trimmed().isEmpty())
                        return false;
                    result.appendTypeScreenName(screen.trimmed());
                }
            }
            break;
        }

        case mouseDown:
        case mouseUp:
        case mousebutton: {
            KeySequence sequence;
            if (!KeySequence::fromString(args, true, sequence))
                return false;
            result.setType(type - mouseDown);
            result.setKeySequence(sequence);
            break;
        }

        case switchToScreen:
            if (args.isEmpty())
                return false;
            result.setType(type);
            result.setSwitchScreenName(args);
            break;

        case toggleScreen:
            if (hasArgs)
                return false;
            result.setType(type);
            break;

        case switchInDirection: {
            int direction = indexOfName(switch_direction_names_, switchDown + 1, args);
            if (direction < 0)
                return false;
            result.setType(type);
            result.setSwitchDirection(direction);
            break;
        }

        case lockCursorToScreen: {
            int mode = indexOfName(lock_cursor_mode_names_, lockCursorOff + 1, args);
            if (mode < 0)
                return false;
            result.setType(type);
            result.setLockCursorMode(mode);
            break;
        }

        default:
            return false;
    }

    action = result;
    return true;
}
