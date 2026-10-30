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

#include "ServerConfig.h"
#include "Hotkey.h"
#include "MainWindow.h"
#include "AddClientDialog.h"

#include "server/Config.h"
#include "inputleap/key_types.h"

#include <QtCore>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>

namespace {

using CoreConfig = inputleap::Config;
using Options = CoreConfig::ScreenOptions;

const char kNewFileHeader[] =
    "# Leapdesk KVM server configuration.\n"
    "# Written by the Leapdesk KVM settings window; comments other than this\n"
    "# header are not kept when it saves.\n"
    "\n";

bool parseConfig(const QString& text, CoreConfig& config, QString* error)
{
    std::istringstream in(text.toStdString());
    try {
        CoreConfig parsed;
        in >> parsed;
        config = parsed;
        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromStdString(e.what());
        return false;
    }
}

QString formatConfig(const CoreConfig& config)
{
    std::ostringstream out;
    out << config;
    return QString::fromStdString(out.str());
}

// The comment block a file starts with, kept when saving so that notes like
// "how to reload this" survive; comments further down are lost.
QString leadingComments(const QString& text)
{
    QString header;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('#')))
            break;
        header += line + QLatin1Char('\n');
    }
    // the split yields one extra empty piece for a file that is all comments
    if (!header.isEmpty() && header == text + QLatin1Char('\n'))
        header.chop(1);
    return header;
}

// Percentages the way the file writes them, with the same float rounding as
// the parser so that an unchanged link compares equal after a round trip.
double toPercent(float fraction)
{
    return std::round(fraction * 10000.0) / 100.0;
}

float toFraction(double percent)
{
    return static_cast<float>(percent / 100.0);
}

inputleap::EDirection toDirection(ScreenLink::Side side)
{
    switch (side) {
        case ScreenLink::Side::Left: return inputleap::kLeft;
        case ScreenLink::Side::Right: return inputleap::kRight;
        case ScreenLink::Side::Up: return inputleap::kTop;
        case ScreenLink::Side::Down: return inputleap::kBottom;
    }
    return inputleap::kNoDirection;
}

ScreenLink::Side toSide(inputleap::EDirection direction)
{
    switch (direction) {
        case inputleap::kLeft: return ScreenLink::Side::Left;
        case inputleap::kTop: return ScreenLink::Side::Up;
        case inputleap::kBottom: return ScreenLink::Side::Down;
        default: return ScreenLink::Side::Right;
    }
}

bool findOption(const Options& options, OptionID id, OptionValue& value)
{
    auto it = options.find(id);
    if (it == options.end())
        return false;
    value = it->second;
    return true;
}

// The options the settings window would write for these lines of text, as the
// server reads them. Comparing two of these tells what an edit changed.
bool parseOptions(const QString& sectionText, const std::string& screenName,
                  Options& options, QString* error)
{
    CoreConfig config;
    if (!parseConfig(sectionText, config, error))
        return false;
    const Options* parsed = config.getOptions(screenName);
    options = parsed ? *parsed : Options();
    return true;
}

bool screenOptions(const Screen& screen, Options& options, QString* error)
{
    QString text;
    QTextStream stream(&text);
    stream << "section: screens\n";
    screen.writeScreensSection(stream);
    stream << "end\n";
    stream.flush();
    return parseOptions(text, screen.name().toStdString(), options, error);
}

// Sets what differs between before and after on the named screen (or the
// global options for an empty name), leaving every other option alone.
void applyOptionChanges(CoreConfig& config, const std::string& name,
                        const Options& before, const Options& after)
{
    for (const auto& option : after) {
        auto it = before.find(option.first);
        if (it == before.end() || it->second != option.second)
            config.addOption(name, option.first, option.second);
    }
    for (const auto& option : before) {
        if (after.count(option.first) == 0)
            config.removeOption(name, option.first);
    }
}

QString screensSection(const std::vector<Screen>& screens)
{
    QString text = QStringLiteral("section: screens\n");
    for (const Screen& screen : screens)
        text += QStringLiteral("\t%1:\n").arg(screen.name());
    return text + QStringLiteral("end\n");
}

QString hotkeyLine(const Hotkey& hotkey)
{
    QString text;
    QTextStream stream(&text);
    stream << hotkey;
    stream.flush();
    return text.trimmed();
}

// Parses hotkey lines the way the server would, for the given screens.
bool parseRules(const QStringList& lines, const std::vector<Screen>& screens,
                std::vector<inputleap::InputFilter::Rule>& rules, QString* error)
{
    QString text = screensSection(screens) + QStringLiteral("section: options\n");
    for (const QString& line : lines)
        text += QLatin1Char('\t') + line + QLatin1Char('\n');
    text += QStringLiteral("end\n");

    CoreConfig config;
    if (!parseConfig(text, config, error))
        return false;
    rules = config.get_input_filter_rules();
    return true;
}

const OptionID kModifierOptions[] = {
    kOptionModifierMapForShift, kOptionModifierMapForControl, kOptionModifierMapForAlt,
    kOptionModifierMapForMeta, kOptionModifierMapForSuper
};

const OptionID kFixOptions[] = {
    kOptionHalfDuplexCapsLock, kOptionHalfDuplexNumLock, kOptionHalfDuplexScrollLock,
    kOptionXTestXineramaUnaware, kOptionScreenPreserveFocus
};

} // namespace

ServerConfig::ServerConfig(QSettings* settings, QString serverName, MainWindow* mainWindow) :
    m_pSettings(settings),
    m_ServerName(serverName),
    m_IgnoreAutoConfigClient(false),
    m_EnableDragAndDrop(false),
    m_pMainWindow(mainWindow)
{
    Q_ASSERT(m_pSettings);

    for (int i = 0; i < static_cast<int>(SwitchCorner::Count); i++)
        m_SwitchCorners << false;

    settings->beginGroup("internalConfig");
    m_IgnoreAutoConfigClient = settings->value("ignoreAutoConfigClient").toBool();
    m_EnableDragAndDrop = settings->value("enableDragAndDrop", true).toBool();
    settings->endGroup();

    reset();
}

ServerConfig::~ServerConfig()
{
    saveSettings();
}

void ServerConfig::saveSettings()
{
    // only the settings window's own preferences; the configuration is in the file
    settings().beginGroup("internalConfig");
    settings().setValue("ignoreAutoConfigClient", ignoreAutoConfigClient());
    settings().setValue("enableDragAndDrop", enableDragAndDrop());
    settings().endGroup();
}

void ServerConfig::resetOptions()
{
    m_HasHeartbeat = false;
    m_Heartbeat = 5000;
    m_RelativeMouseMoves = false;
    m_ScreenSaverSync = true;
    m_Win32KeepForeground = false;
    m_HasSwitchDelay = false;
    m_SwitchDelay = 250;
    m_HasSwitchDoubleTap = false;
    m_SwitchDoubleTap = 250;
    m_SwitchCornerSize = 0;
    for (int i = 0; i < m_SwitchCorners.size(); i++)
        m_SwitchCorners[i] = false;
    m_ClipboardSharing = true;
    m_ClipboardSharingSize = defaultClipboardSharingSize();
}

void ServerConfig::reset()
{
    resetOptions();
    m_Screens.clear();
    m_Links.clear();
    m_Hotkeys.clear();
    m_PreservedRules.clear();

    m_Baseline.reset();
    m_BaselineText.clear();
    m_HeaderComment = QString::fromLatin1(kNewFileHeader);

    // nothing is on disk yet, so everything but the defaults counts as an edit
    m_LoadedScreens.clear();
    m_LoadedLinks.clear();
    m_LoadedOptions = optionsText();
    m_LoadedHotkeys.clear();

    if (!m_ServerName.isEmpty())
        m_Screens.push_back(Screen(m_ServerName));
}

void ServerConfig::markLoaded()
{
    for (Screen& screen : m_Screens)
        screen.setOriginalName(screen.name());
    m_LoadedScreens = m_Screens;
    m_LoadedLinks = m_Links;
    m_LoadedOptions = optionsText();
    m_LoadedHotkeys = hotkeyLines();
}

bool ServerConfig::load(const QString& fileName, QString* error)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());

    CoreConfig config;
    if (!parseConfig(text, config, error))
        return false;

    resetOptions();
    m_Screens.clear();
    m_Links.clear();
    m_Hotkeys.clear();
    m_PreservedRules.clear();

    for (auto name = config.begin(); name != config.end(); ++name) {
        Screen screen(QString::fromStdString(*name));
        const Options* options = config.getOptions(*name);
        if (options) {
            OptionValue value;
            for (int i = 0; i < static_cast<int>(std::size(kModifierOptions)); i++) {
                if (!findOption(*options, kModifierOptions[i], value))
                    continue;
                if (value == kKeyModifierIDNull)
                    screen.setModifier(static_cast<Modifier>(i), Modifier::None);
                else if (value >= kKeyModifierIDShift && value <= kKeyModifierIDSuper)
                    screen.setModifier(static_cast<Modifier>(i), static_cast<Modifier>(value - 1));
                // AltGr can't be picked here; save() keeps the file's value for it
            }
            for (int i = 0; i < static_cast<int>(std::size(kFixOptions)); i++) {
                if (findOption(*options, kFixOptions[i], value))
                    screen.setFix(static_cast<Fix>(i), value != 0);
            }
            if (findOption(*options, kOptionScreenSwitchCorners, value)) {
                for (int i = 0; i < static_cast<int>(SwitchCorner::Count); i++)
                    screen.setSwitchCorner(static_cast<SwitchCorner>(i), (value & (1 << i)) != 0);
            }
            if (findOption(*options, kOptionScreenSwitchCornerSize, value))
                screen.setSwitchCornerSize(value);
        }
        m_Screens.push_back(screen);
    }

    for (auto alias = config.beginAll(); alias != config.endAll(); ++alias) {
        if (alias->first == alias->second)
            continue;
        QString canonical = QString::fromStdString(alias->second);
        for (Screen& screen : m_Screens) {
            if (screen.name() == canonical)
                screen.addAlias(QString::fromStdString(alias->first));
        }
    }

    for (const Screen& screen : m_Screens) {
        std::string name = screen.name().toStdString();
        for (auto link = config.beginNeighbor(name); link != config.endNeighbor(name); ++link) {
            ScreenLink entry;
            entry.source = screen.name();
            entry.side = toSide(link->first.getSide());
            entry.sourceStart = toPercent(link->first.getInterval().first);
            entry.sourceEnd = toPercent(link->first.getInterval().second);
            entry.target = QString::fromStdString(link->second.getName());
            entry.targetStart = toPercent(link->second.getInterval().first);
            entry.targetEnd = toPercent(link->second.getInterval().second);
            m_Links.push_back(entry);
        }
    }

    if (const Options* options = config.getOptions("")) {
        OptionValue value;
        if (findOption(*options, kOptionHeartbeat, value)) {
            m_HasHeartbeat = true;
            m_Heartbeat = value;
        }
        if (findOption(*options, kOptionRelativeMouseMoves, value))
            m_RelativeMouseMoves = value != 0;
        if (findOption(*options, kOptionScreenSaverSync, value))
            m_ScreenSaverSync = value != 0;
        if (findOption(*options, kOptionWin32KeepForeground, value))
            m_Win32KeepForeground = value != 0;
        if (findOption(*options, kOptionScreenSwitchDelay, value)) {
            m_HasSwitchDelay = true;
            m_SwitchDelay = value;
        }
        if (findOption(*options, kOptionScreenSwitchTwoTap, value)) {
            m_HasSwitchDoubleTap = true;
            m_SwitchDoubleTap = value;
        }
        if (findOption(*options, kOptionScreenSwitchCorners, value)) {
            for (int i = 0; i < m_SwitchCorners.size(); i++)
                m_SwitchCorners[i] = (value & (1 << i)) != 0;
        }
        if (findOption(*options, kOptionScreenSwitchCornerSize, value))
            m_SwitchCornerSize = value;
        if (findOption(*options, kOptionClipboardSharing, value))
            m_ClipboardSharing = value != 0;
        if (findOption(*options, kOptionClipboardSharingSize, value))
            m_ClipboardSharingSize = static_cast<size_t>(value);
    }

    // a rule is editable only if the hotkey editor writes it back exactly as
    // the server read it; anything else is kept verbatim
    for (const auto& rule : config.get_input_filter_rules()) {
        QString text = QString::fromStdString(rule.format());
        Hotkey hotkey;
        bool editable = Hotkey::fromText(text, hotkey);
        if (editable) {
            std::vector<inputleap::InputFilter::Rule> reparsed;
            editable = parseRules({hotkeyLine(hotkey)}, m_Screens, reparsed, nullptr) &&
                       reparsed.size() == 1 && reparsed[0].format() == rule.format();
        }
        if (editable)
            m_Hotkeys.push_back(hotkey);
        else
            m_PreservedRules.append(text);
    }

    m_Baseline = std::make_shared<const CoreConfig>(config);
    m_BaselineText = text;
    m_HeaderComment = leadingComments(text);
    markLoaded();
    return true;
}

bool ServerConfig::loadLegacySettings()
{
    settings().beginGroup("internalConfig");
    if (!settings().contains("numColumns")) {
        settings().endGroup();
        return false;
    }

    reset();
    m_Screens.clear();

    int columns = settings().value("numColumns", 5).toInt();
    int rows = settings().value("numRows", 3).toInt();

    std::vector<Screen> grid(static_cast<std::size_t>(columns * rows));
    int numScreens = settings().beginReadArray("screens");
    for (int i = 0; i < numScreens && i < static_cast<int>(grid.size()); i++) {
        settings().setArrayIndex(i);
        grid[i].loadSettings(settings());
    }
    settings().endArray();

    m_HasHeartbeat = settings().value("hasHeartbeat", false).toBool();
    m_Heartbeat = settings().value("heartbeat", 5000).toInt();
    m_RelativeMouseMoves = settings().value("relativeMouseMoves", false).toBool();
    m_ScreenSaverSync = settings().value("screenSaverSync", true).toBool();
    m_Win32KeepForeground = settings().value("win32KeepForeground", false).toBool();
    m_HasSwitchDelay = settings().value("hasSwitchDelay", false).toBool();
    m_SwitchDelay = settings().value("switchDelay", 250).toInt();
    m_HasSwitchDoubleTap = settings().value("hasSwitchDoubleTap", false).toBool();
    m_SwitchDoubleTap = settings().value("switchDoubleTap", 250).toInt();
    m_SwitchCornerSize = settings().value("switchCornerSize").toInt();
    m_ClipboardSharing = settings().value("clipboardSharing", true).toBool();
    m_ClipboardSharingSize = settings().value("clipboardSharingSize",
        static_cast<qulonglong>(defaultClipboardSharingSize())).toULongLong();
    readSettings<bool>(settings(), m_SwitchCorners, "switchCorner", false,
                       static_cast<int>(SwitchCorner::Count));

    int numHotkeys = settings().beginReadArray("hotkeys");
    for (int i = 0; i < numHotkeys; i++) {
        settings().setArrayIndex(i);
        Hotkey hotkey;
        hotkey.loadSettings(settings());
        m_Hotkeys.push_back(hotkey);
    }
    settings().endArray();
    settings().endGroup();

    // the grid linked every screen to its direct neighbours, whole edge to whole edge
    static const struct { int dx; int dy; ScreenLink::Side side; } neighbours[] = {
        {  1,  0, ScreenLink::Side::Right },
        { -1,  0, ScreenLink::Side::Left },
        {  0, -1, ScreenLink::Side::Up },
        {  0,  1, ScreenLink::Side::Down },
    };
    for (int i = 0; i < static_cast<int>(grid.size()); i++) {
        if (grid[i].isNull())
            continue;
        m_Screens.push_back(grid[i]);
        for (const auto& n : neighbours) {
            int column = i % columns + n.dx;
            int row = i / columns + n.dy;
            if (column < 0 || column >= columns || row < 0 || row >= rows)
                continue;
            const Screen& neighbour = grid[row * columns + column];
            if (neighbour.isNull())
                continue;
            ScreenLink link;
            link.source = grid[i].name();
            link.side = n.side;
            link.target = neighbour.name();
            m_Links.push_back(link);
        }
    }
    return true;
}

QString ServerConfig::optionsText() const
{
    QString text;
    QTextStream stream(&text);

    if (hasHeartbeat())
        stream << "\theartbeat = " << heartbeat() << "\n";
    stream << "\trelativeMouseMoves = " << (relativeMouseMoves() ? "true" : "false") << "\n";
    stream << "\tscreenSaverSync = " << (screenSaverSync() ? "true" : "false") << "\n";
    stream << "\twin32KeepForeground = " << (win32KeepForeground() ? "true" : "false") << "\n";
    stream << "\tclipboardSharing = " << (clipboardSharing() ? "true" : "false") << "\n";
    stream << "\tclipboardSharingSize = " << clipboardSharingSize() << "\n";
    if (hasSwitchDelay())
        stream << "\tswitchDelay = " << switchDelay() << "\n";
    if (hasSwitchDoubleTap())
        stream << "\tswitchDoubleTap = " << switchDoubleTap() << "\n";

    stream << "\tswitchCorners = none ";
    for (int i = 0; i < switchCorners().size(); i++) {
        if (switchCorners()[i])
            stream << "+" << switchCornerName(static_cast<SwitchCorner>(i)) << " ";
    }
    stream << "\n";
    stream << "\tswitchCornerSize = " << switchCornerSize() << "\n";

    stream.flush();
    return text;
}

QStringList ServerConfig::hotkeyLines() const
{
    QStringList lines;
    for (const Hotkey& hotkey : m_Hotkeys) {
        QString line = hotkeyLine(hotkey);
        if (!line.isEmpty())
            lines.append(line);
    }
    return lines;
}

bool ServerConfig::applyEdits(CoreConfig& config, QString* error) const
{
    auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };

    // screens: removed ones first, so that a new or renamed screen may reuse a name
    for (const Screen& loaded : m_LoadedScreens) {
        bool kept = std::any_of(m_Screens.begin(), m_Screens.end(), [&](const Screen& screen) {
            return screen.originalName() == loaded.name();
        });
        if (!kept)
            config.removeScreen(loaded.name().toStdString());
    }
    for (const Screen& screen : m_Screens) {
        if (screen.originalName().isEmpty() || screen.originalName() == screen.name())
            continue;
        if (!config.renameScreen(screen.originalName().toStdString(), screen.name().toStdString()))
            return fail(QObject::tr("Could not rename screen %1 to %2; is the name already taken?")
                            .arg(screen.originalName(), screen.name()));
    }
    for (const Screen& screen : m_Screens) {
        if (!screen.originalName().isEmpty())
            continue;
        if (!config.addScreen(screen.name().toStdString()))
            return fail(QObject::tr("\"%1\" is not a valid screen name, or is used twice.")
                            .arg(screen.name()));
    }

    // per-screen options and aliases, compared with the screen as loaded
    for (const Screen& screen : m_Screens) {
        Screen before(screen.name());
        for (const Screen& loaded : m_LoadedScreens) {
            if (!screen.originalName().isEmpty() && loaded.name() == screen.originalName()) {
                before = loaded;
                before.setName(screen.name());
            }
        }

        std::string name = screen.name().toStdString();
        Options optionsBefore, optionsAfter;
        QString parseError;
        if (!screenOptions(before, optionsBefore, &parseError) ||
            !screenOptions(screen, optionsAfter, &parseError))
            return fail(QObject::tr("Settings of screen %1: %2").arg(screen.name(), parseError));
        applyOptionChanges(config, name, optionsBefore, optionsAfter);

        if (screen.aliases() != before.aliases()) {
            config.removeAliases(name);
            for (const QString& alias : screen.aliases()) {
                if (!config.addAlias(name, alias.toStdString()))
                    return fail(QObject::tr("Alias \"%1\" of screen %2 is invalid or already in use.")
                                    .arg(alias, screen.name()));
            }
        }
    }

    // links: the list holds every link of the file, so rebuilding it loses nothing
    // the loaded links as they read after this session's renames and removals
    auto currentName = [this](const QString& loadedName, QString* name) {
        for (const Screen& screen : m_Screens) {
            if (!screen.originalName().isEmpty() && screen.originalName() == loadedName) {
                *name = screen.name();
                return true;
            }
        }
        return false;
    };
    std::vector<ScreenLink> loadedLinks;
    for (const ScreenLink& loaded : m_LoadedLinks) {
        ScreenLink link = loaded;
        if (currentName(loaded.source, &link.source) && currentName(loaded.target, &link.target))
            loadedLinks.push_back(link);
    }
    if (m_Links != loadedLinks) {
        for (auto name = config.begin(); name != config.end(); ++name) {
            for (int side = inputleap::kFirstDirection; side <= inputleap::kLastDirection; side++)
                config.disconnect(*name, static_cast<inputleap::EDirection>(side));
        }
        for (const ScreenLink& link : m_Links) {
            if (!config.connect(link.source.toStdString(), toDirection(link.side),
                                toFraction(link.sourceStart), toFraction(link.sourceEnd),
                                link.target.toStdString(),
                                toFraction(link.targetStart), toFraction(link.targetEnd)))
                return fail(QObject::tr("The link from the %1 edge of %2 to %3 overlaps another "
                                        "link on that edge, or names a screen that doesn't exist.")
                                .arg(ScreenLink::sideLabel(link.side),
                                     link.source, link.target));
        }
    }

    // global options
    Options optionsBefore, optionsAfter;
    QString parseError;
    if (!parseOptions(QStringLiteral("section: options\n") + m_LoadedOptions + QStringLiteral("end\n"),
                      "", optionsBefore, &parseError) ||
        !parseOptions(QStringLiteral("section: options\n") + optionsText() + QStringLiteral("end\n"),
                      "", optionsAfter, &parseError))
        return fail(QObject::tr("Options: %1").arg(parseError));
    applyOptionChanges(config, "", optionsBefore, optionsAfter);

    // hotkeys: untouched unless edited, then the kept rules followed by the edited ones
    QStringList lines = hotkeyLines();
    if (lines != m_LoadedHotkeys) {
        std::vector<inputleap::InputFilter::Rule> rules;
        if (!parseRules(m_PreservedRules + lines, m_Screens, rules, &parseError))
            return fail(QObject::tr("Hotkeys: %1").arg(parseError));
        config.get_input_filter_rules() = rules;
    }

    return true;
}

bool ServerConfig::formatFile(QString& text, QString* error) const
{
    CoreConfig config;
    if (m_Baseline)
        config = *m_Baseline;
    if (!applyEdits(config, error))
        return false;

    text = m_HeaderComment + formatConfig(config);

    // what the server will read must be exactly what was edited here
    CoreConfig check;
    QString parseError;
    if (!parseConfig(text, check, &parseError) || check != config) {
        if (error)
            *error = QObject::tr("The configuration did not read back as written (%1). "
                                 "Nothing was saved.").arg(parseError);
        return false;
    }
    return true;
}

static bool writeFile(const QString& fileName, const QString& text, QString* error)
{
    // keep one copy of what was there, in case the result is not what was wanted
    QFile existing(fileName);
    if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString previous = QString::fromUtf8(existing.readAll());
        existing.close();
        if (previous != text) {
            QString backup = fileName + QStringLiteral(".bak");
            QFile::remove(backup);
            QFile source(fileName);
            if (!source.copy(backup)) {
                if (error)
                    *error = QObject::tr("Could not keep a copy of the previous version as %1: %2")
                                 .arg(backup, source.errorString());
                return false;
            }
        }
    }

    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) ||
        file.write(text.toUtf8()) < 0 || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool ServerConfig::save(const QString& fileName, QString* error)
{
    // nothing edited: leave the file as it is, comments and layout included
    if (m_Baseline && !changedOnDisk(fileName)) {
        CoreConfig edited = *m_Baseline;
        if (!applyEdits(edited, error))
            return false;
        if (edited == *m_Baseline) {
            markLoaded();
            return true;
        }
    }

    QString text;
    if (!formatFile(text, error) || !writeFile(fileName, text, error))
        return false;

    auto baseline = std::make_shared<CoreConfig>();
    parseConfig(text, *baseline, nullptr);
    m_Baseline = baseline;
    m_BaselineText = text;
    markLoaded();
    return true;
}

bool ServerConfig::exportTo(const QString& fileName, QString* error) const
{
    QString text;
    return formatFile(text, error) && writeFile(fileName, text, error);
}

bool ServerConfig::changedOnDisk(const QString& fileName) const
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return !m_BaselineText.isEmpty();
    return QString::fromUtf8(file.readAll()) != m_BaselineText;
}

void ServerConfig::renameScreen(const QString& oldName, const QString& newName)
{
    for (Screen& screen : m_Screens) {
        if (screen.name() == oldName)
            screen.setName(newName);
    }
    for (ScreenLink& link : m_Links) {
        if (link.source == oldName)
            link.source = newName;
        if (link.target == oldName)
            link.target = newName;
    }
}

void ServerConfig::ensureServerScreen(const QString& previousName)
{
    auto screen_called = [this](const QString& name) {
        return std::find_if(m_Screens.begin(), m_Screens.end(), [&](const Screen& screen) {
            return screen.name().compare(name, Qt::CaseInsensitive) == 0;
        });
    };

    if (screen_called(m_ServerName) != m_Screens.end())
        return;

    auto previous = previousName.isEmpty() ? m_Screens.end() : screen_called(previousName);
    if (previous != m_Screens.end()) {
        // a copy, since the rename changes the name this refers to
        const QString name = previous->name();
        renameScreen(name, m_ServerName);
    } else {
        m_Screens.push_back(Screen(m_ServerName));
    }
}

void ServerConfig::removeScreen(const QString& name)
{
    m_Screens.erase(std::remove_if(m_Screens.begin(), m_Screens.end(),
                                   [&](const Screen& screen) { return screen.name() == name; }),
                    m_Screens.end());
    m_Links.erase(std::remove_if(m_Links.begin(), m_Links.end(),
                                 [&](const ScreenLink& link) {
                                     return link.source == name || link.target == name;
                                 }),
                  m_Links.end());
}

int ServerConfig::numScreens() const
{
    return static_cast<int>(m_Screens.size());
}

int ServerConfig::autoAddScreen(const QString name)
{
    auto hasScreen = [this](const QString& screenName) {
        return std::any_of(m_Screens.begin(), m_Screens.end(), [&](const Screen& screen) {
            return screen.name().compare(screenName, Qt::CaseInsensitive) == 0;
        });
    };

    if (!hasScreen(m_ServerName)) {
        if (!m_Screens.empty())
            return kAutoAddScreenManualServer;
        m_Screens.push_back(Screen(m_ServerName));
    }
    if (hasScreen(name))
        return kAutoAddScreenIgnore;

    int result = showAddClientDialog(name);
    if (result == kAddClientIgnore)
        return kAutoAddScreenIgnore;

    m_Screens.push_back(Screen(name));
    if (result == kAddClientOther)
        return kAutoAddScreenManualClient;

    ScreenLink link;
    link.source = m_ServerName;
    link.target = name;
    switch (result) {
        case kAddClientLeft: link.side = ScreenLink::Side::Left; break;
        case kAddClientUp: link.side = ScreenLink::Side::Up; break;
        case kAddClientDown: link.side = ScreenLink::Side::Down; break;
        default: link.side = ScreenLink::Side::Right; break;
    }

    bool sideTaken = std::any_of(m_Links.begin(), m_Links.end(), [&](const ScreenLink& other) {
        return other.source == link.source && other.side == link.side;
    });
    if (sideTaken)
        return kAutoAddScreenManualClient;

    m_Links.push_back(link);
    m_Links.push_back(link.reversed());
    return kAutoAddScreenOk;
}

int ServerConfig::showAddClientDialog(const QString& clientName)
{
    int result = kAddClientIgnore;

    if (!m_pMainWindow->isActiveWindow()) {
        m_pMainWindow->showNormal();
        m_pMainWindow->activateWindow();
    }

    AddClientDialog addClientDialog(clientName, m_pMainWindow);
    addClientDialog.exec();
    result = addClientDialog.addResult();
    m_IgnoreAutoConfigClient = addClientDialog.ignoreAutoConfigClient();

    return result;
}

size_t ServerConfig::defaultClipboardSharingSize() {
    return 100 * 1000 * 1000; // 100 MB
}

bool ServerConfig::isValidScreenName(const QString& name)
{
    return CoreConfig().isValidScreenName(name.toStdString());
}

size_t ServerConfig::setClipboardSharingSize(size_t size) {
    using std::swap;
    swap (size, m_ClipboardSharingSize);
    return size;
}
