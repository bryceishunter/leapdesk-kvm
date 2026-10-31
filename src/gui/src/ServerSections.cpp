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

#include "ServerSections.h"

#include "HotkeyDialog.h"
#include "ServerConfig.h"

#include <QCheckBox>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace {

// takes away what build() put in a section, so that it can be built again in another language
void clearBody(QWidget* body)
{
    delete body->layout();
    qDeleteAll(body->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly));
}

QSpinBox* numberBox(QWidget* parent, int minimum, int maximum, int step, const QString& suffix)
{
    auto* box = new QSpinBox(parent);
    box->setRange(minimum, maximum);
    box->setSingleStep(step);
    box->setSuffix(suffix);
    return box;
}

// a check box with the number it turns on beside it
QHBoxLayout* checkWithNumber(QCheckBox* check, QSpinBox* number)
{
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(check);
    row->addWidget(number);
    row->addStretch();
    return row;
}

QLabel* help(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    markMuted(label);
    return label;
}

const qint64 kMegabyte = 1000 * 1000;

} // namespace

ServerOptionsSection::ServerOptionsSection(QWidget* parent) :
    CollapsibleSection(parent)
{
    build();
}

void ServerOptionsSection::build()
{
    setTitle(tr("Server options"));
    QWidget* parent = body();
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto* switching = new QGroupBox(tr("Switching"), parent);
    auto* switching_layout = new QVBoxLayout(switching);
    m_SwitchDelay = new QCheckBox(tr("Switch after the pointer waits at the edge for"), switching);
    m_SwitchDelayTime = numberBox(switching, 10, 10000, 50, tr(" ms"));
    switching_layout->addLayout(checkWithNumber(m_SwitchDelay, m_SwitchDelayTime));
    m_DoubleTap = new QCheckBox(tr("Switch on a double tap within"), switching);
    m_DoubleTapTime = numberBox(switching, 10, 10000, 50, tr(" ms"));
    switching_layout->addLayout(checkWithNumber(m_DoubleTap, m_DoubleTapTime));
    switching_layout->addWidget(help(tr("A double tap is bumping the screen edge twice in quick succession."), switching));

    auto* corners = new QHBoxLayout;
    corners->setSpacing(12);
    corners->addWidget(new QLabel(tr("Dead corners:"), switching));
    const QString corner_names[] = {tr("Top left"), tr("Top right"), tr("Bottom left"), tr("Bottom right")};
    for (int i = 0; i < 4; i++) {
        m_Corners[i] = new QCheckBox(corner_names[i], switching);
        corners->addWidget(m_Corners[i]);
    }
    corners->addStretch();
    switching_layout->addLayout(corners);
    auto* corner_size = new QHBoxLayout;
    auto* corner_label = new QLabel(tr("Corner si&ze:"), switching);
    m_CornerSize = numberBox(switching, 0, 9999, 1, tr(" px"));
    corner_label->setBuddy(m_CornerSize);
    corner_size->addWidget(corner_label);
    corner_size->addWidget(m_CornerSize);
    corner_size->addStretch();
    switching_layout->addLayout(corner_size);
    switching_layout->addWidget(help(tr("The pointer doesn't switch screens from a dead corner. Each screen can "
                                        "add its own in its Screen Settings."), switching));
    layout->addWidget(switching);

    auto* clipboard = new QGroupBox(tr("Clipboard"), parent);
    auto* clipboard_layout = new QVBoxLayout(clipboard);
    m_Clipboard = new QCheckBox(tr("Share the clipboard, up to"), clipboard);
    m_ClipboardSize = numberBox(clipboard, 1, 100, 1, tr(" MB"));
    clipboard_layout->addLayout(checkWithNumber(m_Clipboard, m_ClipboardSize));
#if defined(Q_OS_WIN)
    clipboard_layout->addWidget(help(tr("Copied files go separately, with Taildrop, when you paste them."), clipboard));
#endif
    layout->addWidget(clipboard);

    auto* other = new QGroupBox(tr("Other"), parent);
    auto* other_layout = new QVBoxLayout(other);
    m_RelativeMoves = new QCheckBox(tr("Use relative mouse moves, for games that hold the pointer"), other);
    m_ScreenSaverSync = new QCheckBox(tr("Start and stop screen savers together"), other);
    m_KeepForeground = new QCheckBox(tr("Don't take the foreground window on this computer"), other);
    m_Heartbeat = new QCheckBox(tr("Check clients every"), other);
    m_HeartbeatTime = numberBox(other, 1000, 30000, 1000, tr(" ms"));
    other_layout->addWidget(m_RelativeMoves);
    other_layout->addWidget(m_ScreenSaverSync);
    other_layout->addWidget(m_KeepForeground);
    other_layout->addLayout(checkWithNumber(m_Heartbeat, m_HeartbeatTime));
    layout->addWidget(other);

    for (QCheckBox* check : {m_SwitchDelay, m_DoubleTap, m_Corners[0], m_Corners[1], m_Corners[2], m_Corners[3],
                             m_Clipboard, m_RelativeMoves, m_ScreenSaverSync, m_KeepForeground, m_Heartbeat}) {
        connect(check, &QCheckBox::toggled, this, &ServerOptionsSection::store);
    }
    for (QSpinBox* number : {m_SwitchDelayTime, m_DoubleTapTime, m_CornerSize, m_ClipboardSize, m_HeartbeatTime})
        connect(number, &QSpinBox::valueChanged, this, &ServerOptionsSection::store);

    refresh();
}

void ServerOptionsSection::setConfig(ServerConfig* config)
{
    m_Config = config;
    refresh();
}

void ServerOptionsSection::refresh()
{
    if (m_Config == nullptr)
        return;

    m_Loading = true;
    const ServerConfig& config = *m_Config;
    m_SwitchDelay->setChecked(config.hasSwitchDelay());
    m_SwitchDelayTime->setValue(config.switchDelay());
    m_DoubleTap->setChecked(config.hasSwitchDoubleTap());
    m_DoubleTapTime->setValue(config.switchDoubleTap());
    for (int i = 0; i < 4; i++)
        m_Corners[i]->setChecked(config.switchCorner(static_cast<BaseConfig::SwitchCorner>(i)));
    m_CornerSize->setValue(config.switchCornerSize());
    m_Clipboard->setChecked(config.clipboardSharing());
    m_ClipboardSize->setValue(static_cast<int>(std::lround(static_cast<double>(config.clipboardSharingSize()) / kMegabyte)));
    m_RelativeMoves->setChecked(config.relativeMouseMoves());
    m_ScreenSaverSync->setChecked(config.screenSaverSync());
    m_KeepForeground->setChecked(config.win32KeepForeground());
    m_Heartbeat->setChecked(config.hasHeartbeat());
    m_HeartbeatTime->setValue(config.heartbeat());

    m_SwitchDelayTime->setEnabled(m_SwitchDelay->isChecked());
    m_DoubleTapTime->setEnabled(m_DoubleTap->isChecked());
    m_ClipboardSize->setEnabled(m_Clipboard->isChecked());
    m_HeartbeatTime->setEnabled(m_Heartbeat->isChecked());
    m_Loading = false;
}

void ServerOptionsSection::store()
{
    m_SwitchDelayTime->setEnabled(m_SwitchDelay->isChecked());
    m_DoubleTapTime->setEnabled(m_DoubleTap->isChecked());
    m_ClipboardSize->setEnabled(m_Clipboard->isChecked());
    m_HeartbeatTime->setEnabled(m_Heartbeat->isChecked());
    if (m_Loading || m_Config == nullptr)
        return;

    ServerConfig& config = *m_Config;
    config.haveSwitchDelay(m_SwitchDelay->isChecked());
    config.setSwitchDelay(m_SwitchDelayTime->value());
    config.haveSwitchDoubleTap(m_DoubleTap->isChecked());
    config.setSwitchDoubleTap(m_DoubleTapTime->value());
    for (int i = 0; i < 4; i++)
        config.setSwitchCorner(static_cast<BaseConfig::SwitchCorner>(i), m_Corners[i]->isChecked());
    config.setSwitchCornerSize(m_CornerSize->value());
    config.setClipboardSharing(m_Clipboard->isChecked());
    // a size that isn't a whole number of megabytes stays as it is until the number is changed
    const qint64 shown = static_cast<qint64>(std::lround(static_cast<double>(config.clipboardSharingSize()) / kMegabyte));
    if (m_ClipboardSize->value() != shown)
        config.setClipboardSharingSize(static_cast<size_t>(m_ClipboardSize->value() * kMegabyte));
    config.setRelativeMouseMoves(m_RelativeMoves->isChecked());
    config.setScreenSaverSync(m_ScreenSaverSync->isChecked());
    config.setWin32KeepForeground(m_KeepForeground->isChecked());
    config.haveHeartbeat(m_Heartbeat->isChecked());
    config.setHeartbeat(m_HeartbeatTime->value());
    emit changed();
}

void ServerOptionsSection::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        clearBody(body());
        build();
    }
    CollapsibleSection::changeEvent(event);
}

HotkeysSection::HotkeysSection(QWidget* parent) :
    CollapsibleSection(parent)
{
    build();
}

void HotkeysSection::build()
{
    setTitle(tr("Hotkeys"));
    QWidget* parent = body();
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    layout->addWidget(help(tr("Keys pressed at this computer that switch screens, lock the pointer to a "
                              "screen, or send other keys."), parent));

    m_List = new QWidget(parent);
    layout->addWidget(m_List);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("&New Hotkey..."), parent);
    add->setAutoDefault(false);
    connect(add, &QPushButton::clicked, this, [this]() { edit(-1); });
    buttons->addWidget(add);
    buttons->addStretch();
    layout->addLayout(buttons);

    m_Preserved = new QWidget(parent);
    layout->addWidget(m_Preserved);

    refresh();
}

void HotkeysSection::setConfig(ServerConfig* config)
{
    m_Config = config;
    refresh();
}

void HotkeysSection::refresh()
{
    // later, since this may run because of a button in the list
    for (QWidget* old : m_List->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        old->hide();
        old->deleteLater();
    }
    delete m_List->layout();
    for (QWidget* old : m_Preserved->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        old->deleteLater();
    delete m_Preserved->layout();

    auto* grid = new QGridLayout(m_List);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);

    const auto& hotkeys = m_Config != nullptr ? std::as_const(*m_Config).hotkeys() : std::vector<Hotkey>();
    if (hotkeys.empty()) {
        grid->addWidget(help(tr("No hotkeys yet."), m_List), 0, 0, 1, 4);
    }
    for (int i = 0; i < static_cast<int>(hotkeys.size()); i++) {
        const Hotkey& hotkey = hotkeys[i];
        auto* keys = new QLabel(hotkey.keySequence().toString(), m_List);
        keys->setTextFormat(Qt::PlainText);
        QFont font = monospaceFont(keys->font());
        font.setWeight(QFont::DemiBold);
        keys->setFont(font);
        grid->addWidget(keys, i, 0, Qt::AlignTop);

        QStringList what;
        for (const Action& action : hotkey.actions())
            what << action.description();
        auto* actions = new QLabel(what.join(QStringLiteral("; ")), m_List);
        actions->setTextFormat(Qt::PlainText);
        actions->setWordWrap(true);
        grid->addWidget(actions, i, 1, Qt::AlignTop);

        auto* change = new QPushButton(tr("Edit..."), m_List);
        change->setAccessibleName(tr("Edit %1...").arg(hotkey.keySequence().toString()));
        change->setAutoDefault(false);
        connect(change, &QPushButton::clicked, this, [this, i]() { edit(i); });
        grid->addWidget(change, i, 2);
        auto* drop = new QPushButton(tr("Remove"), m_List);
        drop->setAccessibleName(tr("Remove %1").arg(hotkey.keySequence().toString()));
        drop->setAutoDefault(false);
        connect(drop, &QPushButton::clicked, this, [this, i]() { remove(i); });
        grid->addWidget(drop, i, 3);
    }

    auto* preserved = new QVBoxLayout(m_Preserved);
    preserved->setContentsMargins(0, 0, 0, 0);
    const QStringList rules = m_Config != nullptr ? m_Config->preservedRules() : QStringList();
    m_Preserved->setVisible(!rules.isEmpty());
    if (!rules.isEmpty()) {
        preserved->addWidget(help(tr("These rules in the layout file can't be edited here, and are kept as they are:"),
                                  m_Preserved));
        auto* text = new QLabel(rules.join('\n'), m_Preserved);
        text->setTextFormat(Qt::PlainText);
        text->setFont(monospaceFont(text->font()));
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        preserved->addWidget(text);
    }
}

void HotkeysSection::edit(int index)
{
    if (m_Config == nullptr)
        return;
    auto& hotkeys = m_Config->hotkeys();
    if (index >= static_cast<int>(hotkeys.size()))
        return;

    Hotkey hotkey = index < 0 ? Hotkey() : hotkeys[index];
    HotkeyDialog dialog(this, *m_Config, hotkey);
    if (dialog.exec() != QDialog::Accepted)
        return;

    if (index < 0)
        hotkeys.push_back(hotkey);
    else
        hotkeys[index] = hotkey;
    refresh();
    emit changed();
}

void HotkeysSection::remove(int index)
{
    if (m_Config == nullptr || index < 0 || index >= static_cast<int>(m_Config->hotkeys().size()))
        return;

    auto& hotkeys = m_Config->hotkeys();
    const QString keys = hotkeys[index].keySequence().toString();
    if (QMessageBox::question(this, tr("Remove hotkey"), tr("Remove the hotkey %1?").arg(keys)) != QMessageBox::Yes)
        return;
    hotkeys.erase(hotkeys.begin() + index);
    refresh();
    emit changed();
}

void HotkeysSection::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        clearBody(body());
        build();
    }
    CollapsibleSection::changeEvent(event);
}
