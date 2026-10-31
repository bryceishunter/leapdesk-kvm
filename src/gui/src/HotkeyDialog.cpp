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

#include "HotkeyDialog.h"
#include "ui_HotkeyDialog.h"

#include "ActionDialog.h"
#include <KeySequenceWidget.h>

#include <QtCore>
#include <QtGui>
#include <QMessageBox>

#include <algorithm>

HotkeyDialog::HotkeyDialog (QWidget* parent, const ServerConfig& config, Hotkey& hotkey) :
    QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
    ui_{std::make_unique<Ui::HotkeyDialog>()},
    m_Config(config),
    m_Hotkey(hotkey),
    m_Draft(hotkey)
{
    ui_->setupUi(this);

    // the keys as they're pressed, and the hotkey's own keys to start with, so that it can be
    // accepted without pressing them again
    KeySequenceWidget* keys = ui_->m_pKeySequenceWidgetHotkey;
    keys->setKeyPrefix(QString());
    keys->setKeyPostfix(QString());
    keys->setMousePrefix(tr("Mouse button "));
    keys->setMousePostfix(QString());
    keys->setToolTip(tr("Click, then press the keys."));
    keys->setKeySequence(m_Hotkey.keySequence());

    connect(ui_->m_pButtonNewAction, &QPushButton::clicked, this, [this]() { editAction(-1); });
    connect(ui_->m_pButtonEditAction, &QPushButton::clicked, this, [this]() {
        editAction(ui_->m_pListActions->currentRow());
    });
    connect(ui_->m_pListActions, &QListWidget::itemDoubleClicked, this, [this]() {
        editAction(ui_->m_pListActions->currentRow());
    });
    connect(ui_->m_pButtonRemoveAction, &QPushButton::clicked, this, [this]() {
        const int row = ui_->m_pListActions->currentRow();
        if (row < 0 || row >= static_cast<int>(m_Draft.actions().size()))
            return;
        m_Draft.removeAction(row);
        refreshActions((std::min)(row, static_cast<int>(m_Draft.actions().size()) - 1));
    });
    connect(ui_->m_pListActions, &QListWidget::currentRowChanged, this, [this](int row) {
        ui_->m_pButtonEditAction->setEnabled(row >= 0);
        ui_->m_pButtonRemoveAction->setEnabled(row >= 0);
    });

    refreshActions(m_Draft.actions().empty() ? -1 : 0);
}

void HotkeyDialog::editAction(int index)
{
    const bool add = index < 0;
    if (!add && index >= static_cast<int>(m_Draft.actions().size()))
        return;

    Action action = add ? Action() : m_Draft.actions()[index];
    ActionDialog dialog(this, m_Config, m_Draft, action);
    if (dialog.exec() != QDialog::Accepted)
        return;

    if (add) {
        m_Draft.appendAction(action);
        index = static_cast<int>(m_Draft.actions().size()) - 1;
    } else {
        m_Draft.setAction(index, action);
    }
    refreshActions(index);
}

void HotkeyDialog::refreshActions(int select)
{
    ui_->m_pListActions->clear();
    for (const Action& action : m_Draft.actions())
        ui_->m_pListActions->addItem(action.description());
    ui_->m_pListActions->setCurrentRow(select);
}

void HotkeyDialog::accept()
{
    if (!sequenceWidget()->valid())
        return;

    if (m_Draft.actions().empty()) {
        QMessageBox::information(this, windowTitle(),
                                 tr("Add what the hotkey does first, with New."));
        return;
    }

    m_Draft.setKeySequence(sequenceWidget()->keySequence());
    m_Hotkey = m_Draft;
    QDialog::accept();
}

const KeySequenceWidget *HotkeyDialog::sequenceWidget() const
{
    return ui_->m_pKeySequenceWidgetHotkey;
}

HotkeyDialog::~HotkeyDialog() = default;
