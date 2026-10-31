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

#include "DesktopGeometry.h"
#include "ScreenLink.h"
#include "WindowParts.h"

#include <QFrame>
#include <QList>
#include <QMap>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class ServerConfig;

// A screen of the layout other than the server's own, as the Screens section shows it.
struct LayoutScreen
{
    QString name;
    QString status;         // under its name, such as "Connected", or empty
    bool placed = false;
    ScreenLink placement;   // the link from the server's screen to it, if it's placed
};

// Draws the server's monitors and the screens placed around them. A screen can be dragged onto a
// monitor's outer edge, or out to where screens that aren't placed wait.
class LayoutView : public QWidget
{
    Q_OBJECT

public:
    explicit LayoutView(QWidget* parent = nullptr);

    void setContents(const DesktopGeometry& geometry, const QList<LayoutScreen>& screens);
    void setSelected(const QString& name);

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // what a dragged screen carries
    static const char* kMimeType;

signals:
    void screenSelected(const QString& name);
    // a screen was dropped on the zone at index of the geometry's zones
    void screenDropped(const QString& name, int zone);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // where things are, in desktop pixels
    QRectF screenBox(const LayoutScreen& screen) const;
    QRectF band(const EdgeZone& zone) const;
    QRectF extent() const;
    void fit();
    QString screenAt(const QPointF& pos) const;
    int zoneAt(const QPointF& pos) const;
    // the screen another link from the zone's stretch leads to, other than name
    QString zoneTakenBy(int zone, const QString& name) const;

    DesktopGeometry m_Geometry;
    QList<LayoutScreen> m_Screens;
    QString m_Selected;

    // desktop pixels to the widget's
    double m_Scale = 1;
    QPointF m_Offset;

    QPoint m_PressPos;
    QString m_Pressed;
    // the screen being dragged over this, and the zone it would go to
    QString m_Dragging;
    bool m_OwnDrag = false;
    int m_HoverZone = -1;
};

// Where screens that aren't placed wait, each a chip that can be dragged onto the drawing; a
// placed screen dropped here comes off the layout.
class ScreenTray : public QFrame
{
    Q_OBJECT

public:
    explicit ScreenTray(QWidget* parent = nullptr);
    void setScreens(const QStringList& names, const QString& selected, const QString& emptyText);

signals:
    void screenSelected(const QString& name);
    void screenDropped(const QString& name);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();

    QHBoxLayout* m_Layout;
    QPoint m_PressPos;
    // the chips can't be replaced while one of them is being dragged, so what to show then waits
    bool m_DragFromHere = false;
    QStringList m_Names;
    QString m_SelectedName;
    QString m_EmptyText;
};

// The Screens section: the drawing, the tray, the selected screen's details and the links.
class ScreensSection : public QWidget
{
    Q_OBJECT

public:
    explicit ScreensSection(QWidget* parent = nullptr);

    void setConfig(ServerConfig* config);
    void setMonitors(const QList<QRect>& monitors);
    // the computers this one is paired with, by the names they gave, and what to say about each
    void setComputers(const QStringList& paired, const QMap<QString, QString>& status);
    // shows the configuration again
    void refresh();
    void select(const QString& name);

signals:
    // the configuration was changed and wants saving
    void changed();
    void unpairRequested(const QString& name);

protected:
    void changeEvent(QEvent* event) override;

private:
    void build();
    QList<LayoutScreen> layoutScreens() const;
    bool isPaired(const QString& name) const;
    void refreshInspector();
    void refreshLinks();

    void place(const QString& name, int zone);
    void unplace(const QString& name);
    // from the details: the stretch of the server's edge, and where it enters the other screen
    void stretchEdited();
    void editScreenSettings(const QString& name);
    // edits the link at index, or adds one if it's -1
    void editLink(int index);
    void removeLink(int index);
    // puts link into the list in place of the one at index, or adds it if that's -1; false,
    // after saying so, if it overlaps another
    bool storeLink(const ScreenLink& link, int index);
    // why a stretch of the server's edge can't be used, or empty if it can
    QString explainPlacement(const LayoutScreen& screen) const;

    ServerConfig* m_Config = nullptr;
    DesktopGeometry m_Geometry;
    QStringList m_Paired;
    QMap<QString, QString> m_Status;
    QString m_Selected;
    bool m_Loading = false;

    LayoutView* m_View = nullptr;
    ScreenTray* m_Tray = nullptr;
    QLabel* m_Legend = nullptr;
    QFrame* m_Inspector = nullptr;
    QLabel* m_InspectorName = nullptr;
    QLabel* m_InspectorStatus = nullptr;
    QComboBox* m_Zone = nullptr;
    QWidget* m_Stretch = nullptr;
    QDoubleSpinBox* m_From = nullptr;
    QDoubleSpinBox* m_To = nullptr;
    QLabel* m_StretchUnit = nullptr;
    QDoubleSpinBox* m_TargetFrom = nullptr;
    QDoubleSpinBox* m_TargetTo = nullptr;
    QLabel* m_TargetUnit = nullptr;
    QLabel* m_PlaceText = nullptr;
    QPushButton* m_ScreenSettings = nullptr;
    QPushButton* m_Unpair = nullptr;
    QWidget* m_Links = nullptr;
    QPushButton* m_AddLink = nullptr;
};
