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

#include "ServerConfigDialog.h"
#include <ui_ServerConfigDialog.h>

#include "ServerConfig.h"
#include "HotkeyDialog.h"
#include "ActionDialog.h"
#include "LinkDialog.h"
#include "ScreenLayoutView.h"
#include "ScreenSettingsDialog.h"

#include <QtCore>
#include <QtGui>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>

static QString stretchLabel(double start, double end)
{
    if (start == 0 && end == 100)
        return QObject::tr("whole edge");
    return QObject::tr("%1 to %2 %").arg(QString::number(start), QString::number(end));
}

ServerConfigDialog::ServerConfigDialog(QWidget* parent, ServerConfig& config, const QString& defaultScreenName) :
    QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
    ui_{std::make_unique<Ui::ServerConfigDialog>()},
    m_OrigServerConfig(config),
    m_ServerConfig(config),
    m_Message("")
{
    ui_->setupUi(this);

    // hotkeys and the server's options are edited in the main window; the widgets stay, so
    // that accepting writes back what they were loaded with
    ui_->m_pTabWidget->removeTab(ui_->m_pTabWidget->indexOf(ui_->m_pTabAdvanced));
    ui_->m_pTabWidget->removeTab(ui_->m_pTabWidget->indexOf(ui_->m_pTabHotkeys));
    ui_->m_pTabWidget->setTabBarAutoHide(true);

    ui_->m_pCheckBoxHeartbeat->setChecked(serverConfig().hasHeartbeat());
    ui_->m_pSpinBoxHeartbeat->setValue(serverConfig().heartbeat());

    ui_->m_pCheckBoxRelativeMouseMoves->setChecked(serverConfig().relativeMouseMoves());
    ui_->m_pCheckBoxScreenSaverSync->setChecked(serverConfig().screenSaverSync());
    ui_->m_pCheckBoxWin32KeepForeground->setChecked(serverConfig().win32KeepForeground());

    ui_->m_pCheckBoxSwitchDelay->setChecked(serverConfig().hasSwitchDelay());
    ui_->m_pSpinBoxSwitchDelay->setValue(serverConfig().switchDelay());

    ui_->m_pCheckBoxSwitchDoubleTap->setChecked(serverConfig().hasSwitchDoubleTap());
    ui_->m_pSpinBoxSwitchDoubleTap->setValue(serverConfig().switchDoubleTap());

    ui_->m_pCheckBoxCornerTopLeft->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::TopLeft));
    ui_->m_pCheckBoxCornerTopRight->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::TopRight));
    ui_->m_pCheckBoxCornerBottomLeft->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::BottomLeft));
    ui_->m_pCheckBoxCornerBottomRight->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::BottomRight));
    ui_->m_pSpinBoxSwitchCornerSize->setValue(serverConfig().switchCornerSize());

    ui_->m_pCheckBoxIgnoreAutoConfigClient->setChecked(serverConfig().ignoreAutoConfigClient());

    ui_->m_pCheckBoxEnableDragAndDrop->setChecked(serverConfig().enableDragAndDrop());

    ui_->m_pCheckBoxEnableClipboard->setChecked(serverConfig().clipboardSharing());
    ui_->m_pSpinBoxClipboardSizeLimit->setValue(serverConfig().clipboardSharingSize());
    ui_->m_pSpinBoxClipboardSizeLimit->setEnabled(serverConfig().clipboardSharing());

    for (const Hotkey& hotkey : serverConfig().hotkeys()) {
        ui_->m_pListHotkeys->addItem(hotkey.text());
    }

    ui_->m_pListPreservedRules->addItems(serverConfig().preservedRules());
    ui_->m_pGroupPreservedRules->setVisible(!serverConfig().preservedRules().isEmpty());

    if (serverConfig().numScreens() == 0 && !defaultScreenName.isEmpty())
        serverConfig().screens().push_back(Screen(defaultScreenName));

    ui_->m_pTableLinks->setColumnCount(5);
    ui_->m_pTableLinks->setHorizontalHeaderLabels(
        {tr("From screen"), tr("Edge"), tr("Stretch"), tr("To screen"), tr("Stretch")});
    ui_->m_pTableLinks->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

    // the drawing and the table show the same links; selecting in one selects in the other
    ScreenLayoutView* view = ui_->m_pLayoutView;
    view->setConfiguration(&serverConfig().screens(), &serverConfig().links(),
                           serverConfig().serverName());
    connect(view, &ScreenLayoutView::screenSelected, this, [this] {
        ui_->m_pTableLinks->clearSelection();
        updateScreenButtons();
    });
    connect(view, &ScreenLayoutView::screenActivated, this, [this] {
        updateScreenButtons();
        on_m_pButtonEditScreen_clicked();
    });
    connect(view, &ScreenLayoutView::linkSelected, this, [this](int index) {
        ui_->m_pTableLinks->selectRow(index);
        updateScreenButtons();
    });
    connect(view, &ScreenLayoutView::linkActivated, this, [this](int index) {
        ui_->m_pTableLinks->selectRow(index);
        on_m_pButtonEditLink_clicked();
    });
    connect(view, &ScreenLayoutView::linkEdited, this, [this] { updateLinkRows(); });

    refreshScreens();
    refreshLinks();
}

void ServerConfigDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (!m_Message.isEmpty()) {
        QString message = m_Message;
        m_Message.clear();
        QTimer::singleShot(0, this, [this, message] {
            QMessageBox::information(this, windowTitle(), message);
        });
    }
}

void ServerConfigDialog::accept()
{
    bool hasServerScreen = false;
    for (const Screen& screen : serverConfig().screens()) {
        if (screen.name().compare(serverConfig().serverName(), Qt::CaseInsensitive) == 0)
            hasServerScreen = true;
    }
    if (!hasServerScreen && !serverConfig().serverName().isEmpty()) {
        int answer = QMessageBox::warning(
            this, tr("This computer is missing"),
            tr("None of the screens is named %1, the name of this computer, so the server "
               "will not start with this configuration. Save it anyway?")
                .arg(serverConfig().serverName()),
            QMessageBox::Save | QMessageBox::Cancel);
        if (answer != QMessageBox::Save)
            return;
    }

    serverConfig().haveHeartbeat(ui_->m_pCheckBoxHeartbeat->isChecked());
    serverConfig().setHeartbeat(ui_->m_pSpinBoxHeartbeat->value());

    serverConfig().setRelativeMouseMoves(ui_->m_pCheckBoxRelativeMouseMoves->isChecked());
    serverConfig().setScreenSaverSync(ui_->m_pCheckBoxScreenSaverSync->isChecked());
    serverConfig().setWin32KeepForeground(ui_->m_pCheckBoxWin32KeepForeground->isChecked());

    serverConfig().haveSwitchDelay(ui_->m_pCheckBoxSwitchDelay->isChecked());
    serverConfig().setSwitchDelay(ui_->m_pSpinBoxSwitchDelay->value());

    serverConfig().haveSwitchDoubleTap(ui_->m_pCheckBoxSwitchDoubleTap->isChecked());
    serverConfig().setSwitchDoubleTap(ui_->m_pSpinBoxSwitchDoubleTap->value());

    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::TopLeft,
                                   ui_->m_pCheckBoxCornerTopLeft->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::TopRight,
                                   ui_->m_pCheckBoxCornerTopRight->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::BottomLeft,
                                   ui_->m_pCheckBoxCornerBottomLeft->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::BottomRight,
                                   ui_->m_pCheckBoxCornerBottomRight->isChecked());
    serverConfig().setSwitchCornerSize(ui_->m_pSpinBoxSwitchCornerSize->value());
    serverConfig().setIgnoreAutoConfigClient(ui_->m_pCheckBoxIgnoreAutoConfigClient->isChecked());
    serverConfig().setEnableDragAndDrop(ui_->m_pCheckBoxEnableDragAndDrop->isChecked());
    serverConfig().setClipboardSharing(ui_->m_pCheckBoxEnableClipboard->isChecked());
    serverConfig().setClipboardSharingSize(ui_->m_pSpinBoxClipboardSizeLimit->value());

    // now that the dialog has been accepted, copy the new server config to the original one,
    // which is a reference to the one in MainWindow.
    setOrigServerConfig(serverConfig());

    QDialog::accept();
}

int ServerConfigDialog::selectedLinkRow() const
{
    QModelIndexList rows = ui_->m_pTableLinks->selectionModel()->selectedRows();
    return rows.isEmpty() ? -1 : rows.first().row();
}

QString ServerConfigDialog::selectedScreenName() const
{
    return ui_->m_pLayoutView->selectedScreen();
}

QStringList ServerConfigDialog::screenNames() const
{
    QStringList names;
    for (const Screen& screen : m_ServerConfig.screens())
        names.append(screen.name());
    return names;
}

void ServerConfigDialog::refreshScreens(const QString& select)
{
    ui_->m_pLayoutView->refresh();
    if (!select.isEmpty()) {
        ui_->m_pTableLinks->clearSelection();
        ui_->m_pLayoutView->selectScreen(select);
    }
    updateScreenButtons();
}

void ServerConfigDialog::updateLinkRows()
{
    const auto& links = serverConfig().links();
    ui_->m_pTableLinks->setRowCount(static_cast<int>(links.size()));
    for (int row = 0; row < static_cast<int>(links.size()); row++) {
        const ScreenLink& link = links[row];
        const QString cells[] = {
            link.source, ScreenLink::sideLabel(link.side), stretchLabel(link.sourceStart, link.sourceEnd),
            link.target, stretchLabel(link.targetStart, link.targetEnd),
        };
        for (int column = 0; column < 5; column++) {
            QTableWidgetItem* item = ui_->m_pTableLinks->item(row, column);
            if (item)
                item->setText(cells[column]);
            else
                ui_->m_pTableLinks->setItem(row, column, new QTableWidgetItem(cells[column]));
        }
    }
}

void ServerConfigDialog::refreshLinks(int select)
{
    updateLinkRows();
    ui_->m_pLayoutView->refresh();
    ui_->m_pTableLinks->clearSelection();
    if (select >= 0 && select < static_cast<int>(serverConfig().links().size()))
        ui_->m_pTableLinks->selectRow(select);
    on_m_pTableLinks_itemSelectionChanged();
}

void ServerConfigDialog::updateScreenButtons()
{
    bool selected = !selectedScreenName().isEmpty();
    ui_->m_pButtonEditScreen->setEnabled(selected);
    ui_->m_pButtonRemoveScreen->setEnabled(selected);
}

bool ServerConfigDialog::checkScreenName(const QString& name, const QString& currentName)
{
    if (!ServerConfig::isValidScreenName(name)) {
        QMessageBox::warning(this, tr("Invalid screen name"),
                             tr("\"%1\" is not a valid screen name. Use letters, digits, "
                                "hyphens and underscores, optionally in dot-separated parts "
                                "like a host name.").arg(name));
        return false;
    }
    for (const Screen& screen : serverConfig().screens()) {
        if (screen.name() == currentName)
            continue;
        bool taken = screen.name().compare(name, Qt::CaseInsensitive) == 0 ||
                     screen.aliases().contains(name, Qt::CaseInsensitive);
        if (taken) {
            QMessageBox::warning(this, tr("Screen name taken"),
                                 tr("There already is a screen called %1.").arg(name));
            return false;
        }
    }
    return true;
}

void ServerConfigDialog::on_m_pButtonAddScreen_clicked()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Add screen"),
                                         tr("Screen name (the name the client uses):"),
                                         QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty() || !checkScreenName(name, QString()))
        return;

    serverConfig().screens().push_back(Screen(name));
    refreshScreens(name);
}

void ServerConfigDialog::on_m_pButtonEditScreen_clicked()
{
    QString oldName = selectedScreenName();
    if (oldName.isEmpty())
        return;

    auto& screens = serverConfig().screens();
    auto it = std::find_if(screens.begin(), screens.end(),
                           [&](const Screen& screen) { return screen.name() == oldName; });
    if (it == screens.end())
        return;

    Screen edited = *it;
    ScreenSettingsDialog dlg(this, &edited);
    if (dlg.exec() != QDialog::Accepted)
        return;

    QString newName = edited.name();
    if (newName != oldName && !checkScreenName(newName, oldName))
        return;

    serverConfig().renameScreen(oldName, newName);
    for (Screen& screen : screens) {
        if (screen.name() == newName)
            screen = edited;
    }
    refreshScreens(newName);
    refreshLinks();
}

void ServerConfigDialog::on_m_pButtonRemoveScreen_clicked()
{
    QString name = selectedScreenName();
    if (name.isEmpty())
        return;

    if (name.compare(serverConfig().serverName(), Qt::CaseInsensitive) == 0) {
        QMessageBox::information(this, tr("Remove screen"),
                                 tr("%1 is this computer; the server needs it in its "
                                    "configuration.").arg(name));
        return;
    }

    int linkCount = 0;
    for (const ScreenLink& link : serverConfig().links()) {
        if (link.source == name || link.target == name)
            linkCount++;
    }
    QString question = linkCount == 0
        ? tr("Remove %1?").arg(name)
        : tr("Remove %1 and the %n link(s) to and from it?", nullptr, linkCount).arg(name);
    if (QMessageBox::question(this, tr("Remove screen"), question) != QMessageBox::Yes)
        return;

    serverConfig().removeScreen(name);
    refreshScreens();
    refreshLinks();
}

int ServerConfigDialog::findLink(const QString& source, ScreenLink::Side side,
                                 double start, double end) const
{
    const auto& links = m_ServerConfig.links();
    for (int i = 0; i < static_cast<int>(links.size()); i++) {
        const ScreenLink& link = links[i];
        if (link.source == source && link.side == side &&
            link.sourceStart == start && link.sourceEnd == end)
            return i;
    }
    return -1;
}

bool ServerConfigDialog::storeLink(const ScreenLink& link, int index)
{
    auto& links = serverConfig().links();
    for (int i = 0; i < static_cast<int>(links.size()); i++) {
        const ScreenLink& other = links[i];
        if (i == index || other.source != link.source || other.side != link.side)
            continue;
        if (link.sourceStart < other.sourceEnd && other.sourceStart < link.sourceEnd) {
            QMessageBox::warning(
                this, tr("Links overlap"),
                tr("The %1 edge of %2 already leads to %3 along %4. A stretch of an edge can "
                   "lead to only one place; shorten one of the two links.")
                    .arg(ScreenLink::sideLabel(link.side), link.source, other.target,
                         stretchLabel(other.sourceStart, other.sourceEnd)));
            return false;
        }
    }

    if (index < 0)
        links.push_back(link);
    else
        links[index] = link;
    return true;
}

void ServerConfigDialog::on_m_pTableLinks_itemSelectionChanged()
{
    int row = selectedLinkRow();
    ui_->m_pButtonEditLink->setEnabled(row >= 0);
    ui_->m_pButtonRemoveLink->setEnabled(row >= 0);
    if (row >= 0 || ui_->m_pLayoutView->selectedLink() >= 0)
        ui_->m_pLayoutView->selectLink(row);
    updateScreenButtons();
}

void ServerConfigDialog::on_m_pTableLinks_cellDoubleClicked()
{
    on_m_pButtonEditLink_clicked();
}

void ServerConfigDialog::on_m_pButtonAddLink_clicked()
{
    QStringList names = screenNames();
    if (names.size() < 1)
        return;

    ScreenLink link;
    link.source = selectedScreenName().isEmpty() ? names.first() : selectedScreenName();
    link.target = names.first();
    for (const QString& name : names) {
        if (name != link.source) {
            link.target = name;
            break;
        }
    }

    LinkDialog dlg(this, names, link, true, false);
    if (dlg.exec() != QDialog::Accepted)
        return;

    link = dlg.link();
    if (!storeLink(link, -1))
        return;
    if (dlg.includeLinkBack())
        storeLink(link.reversed(), -1);
    refreshLinks(static_cast<int>(serverConfig().links().size()) - 1);
}

void ServerConfigDialog::on_m_pButtonEditLink_clicked()
{
    int row = selectedLinkRow();
    if (row < 0 || row >= static_cast<int>(serverConfig().links().size()))
        return;

    ScreenLink old = serverConfig().links()[row];
    ScreenLink oldBack = old.reversed();
    int backIndex = findLink(oldBack.source, oldBack.side, oldBack.sourceStart, oldBack.sourceEnd);
    if (backIndex >= 0 && serverConfig().links()[backIndex] != oldBack)
        backIndex = -1;

    LinkDialog dlg(this, screenNames(), old, false, backIndex >= 0);
    if (dlg.exec() != QDialog::Accepted)
        return;

    ScreenLink link = dlg.link();
    if (!storeLink(link, row))
        return;
    if (dlg.includeLinkBack())
        storeLink(link.reversed(), backIndex);
    refreshLinks(row);
}

void ServerConfigDialog::on_m_pButtonRemoveLink_clicked()
{
    int row = selectedLinkRow();
    auto& links = serverConfig().links();
    if (row < 0 || row >= static_cast<int>(links.size()))
        return;

    ScreenLink back = links[row].reversed();
    auto backIt = std::find(links.begin(), links.end(), back);
    bool removeBack = false;
    if (backIt != links.end() && backIt - links.begin() != row) {
        int answer = QMessageBox::question(
            this, tr("Remove link"),
            tr("Also remove the way back, from the %1 edge of %2 to %3?")
                .arg(ScreenLink::sideLabel(back.side), back.source, back.target),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
            return;
        removeBack = answer == QMessageBox::Yes;
    }

    ScreenLink removed = links[row];
    links.erase(links.begin() + row);
    if (removeBack)
        links.erase(std::find(links.begin(), links.end(), removed.reversed()));
    refreshLinks(std::min(row, static_cast<int>(links.size()) - 1));
}

void ServerConfigDialog::on_m_pButtonNewHotkey_clicked()
{
    Hotkey hotkey;
    HotkeyDialog dlg(this, serverConfig(), hotkey);
    if (dlg.exec() == QDialog::Accepted)
    {
        serverConfig().hotkeys().push_back(hotkey);
        ui_->m_pListHotkeys->addItem(hotkey.text());
    }
}

void ServerConfigDialog::on_m_pButtonEditHotkey_clicked()
{
    int idx = ui_->m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < static_cast<int>(serverConfig().hotkeys().size()));
    Hotkey& hotkey = serverConfig().hotkeys()[idx];
    HotkeyDialog dlg(this, serverConfig(), hotkey);
    if (dlg.exec() == QDialog::Accepted)
        ui_->m_pListHotkeys->currentItem()->setText(hotkey.text());
}

void ServerConfigDialog::on_m_pButtonRemoveHotkey_clicked()
{
    int idx = ui_->m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < static_cast<int>(serverConfig().hotkeys().size()));
    serverConfig().hotkeys().erase(serverConfig().hotkeys().begin() + idx);
    ui_->m_pListActions->clear();
    delete ui_->m_pListHotkeys->item(idx);
}

void ServerConfigDialog::on_m_pListHotkeys_itemSelectionChanged()
{
    bool itemsSelected = !ui_->m_pListHotkeys->selectedItems().isEmpty();
    ui_->m_pButtonEditHotkey->setEnabled(itemsSelected);
    ui_->m_pButtonRemoveHotkey->setEnabled(itemsSelected);
    ui_->m_pButtonNewAction->setEnabled(itemsSelected);

    if (itemsSelected && serverConfig().hotkeys().size() > 0)
    {
        ui_->m_pListActions->clear();

        int idx = ui_->m_pListHotkeys->row(ui_->m_pListHotkeys->selectedItems()[0]);

        // There's a bug somewhere around here: We get idx == 1 right after we deleted the next to last item, so idx can
        // only possibly be 0. GDB shows we got called indirectly from the delete line in
        // on_m_pButtonRemoveHotkey_clicked() above, but the delete is of course necessary and seems correct.
        // The while() is a generalized workaround for all that and shouldn't be required.
        while (idx >= 0 && idx >= static_cast<int>(serverConfig().hotkeys().size()))
            idx--;

        Q_ASSERT(idx >= 0 && idx < static_cast<int>(serverConfig().hotkeys().size()));

        const Hotkey& hotkey = serverConfig().hotkeys()[idx];
        for (const Action& action : hotkey.actions()) {
            ui_->m_pListActions->addItem(action.text());
        }
    }
}

void ServerConfigDialog::on_m_pButtonNewAction_clicked()
{
    int idx = ui_->m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < static_cast<int>(serverConfig().hotkeys().size()));
    Hotkey& hotkey = serverConfig().hotkeys()[idx];

    Action action;
    ActionDialog dlg(this, serverConfig(), hotkey, action);
    if (dlg.exec() == QDialog::Accepted)
    {
        hotkey.appendAction(action);
        ui_->m_pListActions->addItem(action.text());
    }
}

void ServerConfigDialog::on_m_pButtonEditAction_clicked()
{
    int idxHotkey = ui_->m_pListHotkeys->currentRow();
    Q_ASSERT(idxHotkey >= 0 && idxHotkey < static_cast<int>(serverConfig().hotkeys().size()));
    Hotkey& hotkey = serverConfig().hotkeys()[idxHotkey];

    int idxAction = ui_->m_pListActions->currentRow();
    Q_ASSERT(idxAction >= 0 && idxAction < static_cast<int>(hotkey.actions().size()));
    Action action = hotkey.actions()[idxAction];

    ActionDialog dlg(this, serverConfig(), hotkey, action);
    if (dlg.exec() == QDialog::Accepted) {
        hotkey.setAction(idxAction, action);
        ui_->m_pListActions->currentItem()->setText(action.text());
    }
}

void ServerConfigDialog::on_m_pButtonRemoveAction_clicked()
{
    int idxHotkey = ui_->m_pListHotkeys->currentRow();
    Q_ASSERT(idxHotkey >= 0 && idxHotkey < static_cast<int>(serverConfig().hotkeys().size()));
    Hotkey& hotkey = serverConfig().hotkeys()[idxHotkey];

    int idxAction = ui_->m_pListActions->currentRow();
    Q_ASSERT(idxAction >= 0 && idxAction < static_cast<int>(hotkey.actions().size()));

    hotkey.removeAction(idxAction);
    delete ui_->m_pListActions->currentItem();
}

void ServerConfigDialog::on_m_pListActions_itemSelectionChanged()
{
    ui_->m_pButtonEditAction->setEnabled(!ui_->m_pListActions->selectedItems().isEmpty());
    ui_->m_pButtonRemoveAction->setEnabled(!ui_->m_pListActions->selectedItems().isEmpty());
}

void ServerConfigDialog::on_m_pCheckBoxEnableClipboard_stateChanged(int state)
{
    ui_->m_pSpinBoxClipboardSizeLimit->setEnabled(state == Qt::Checked);
}

ServerConfigDialog::~ServerConfigDialog() = default;
