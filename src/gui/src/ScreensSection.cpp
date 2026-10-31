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

#include "ScreensSection.h"

#include "LinkDialog.h"
#include "ScreenSettingsDialog.h"
#include "ServerConfig.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

using Side = ScreenLink::Side;

bool sameName(const QString& a, const QString& b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

QColor blend(const QColor& from, const QColor& to, double amount)
{
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

QColor accentOf(const QPalette& palette)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    return palette.color(QPalette::Accent);
#else
    return palette.color(QPalette::Highlight);
#endif
}

// percentages as the file has them, to two places
double rounded(double percent)
{
    return std::round(percent * 100) / 100;
}

QString percent(double value)
{
    return QString::number(rounded(value));
}

bool isWhole(double from, double to)
{
    return from <= 0.005 && to >= 99.995;
}

// a stretch of an edge, in words
QString stretchWords(double from, double to)
{
    return isWhole(from, to) ? ScreensSection::tr("the whole edge")
                             : ScreensSection::tr("%1 to %2 %").arg(percent(from), percent(to));
}

QString zoneWords(const EdgeZone& zone)
{
    const QString side = ScreenLink::sideLabel(zone.side);
    return zone.combined()
        ? ScreensSection::tr("All monitors, %1 edge (%2)").arg(side, stretchWords(zone.from, zone.to))
        : ScreensSection::tr("Monitor %1, %2 edge (%3)").arg(zone.monitor).arg(side, stretchWords(zone.from, zone.to));
}

// what a client looks like when placed: wider than tall, as most screens are
const double kClientAspect = 1.6;

QPixmap dragPicture(const QString& name, const QWidget* widget)
{
    const QFontMetrics metrics = widget->fontMetrics();
    const qreal ratio = widget->devicePixelRatioF();
    const QSize size(metrics.horizontalAdvance(name) + 2 * metrics.height(), metrics.height() * 2);
    QPixmap pixmap(size * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = widget->palette();
    painter.setPen(QPen(accentOf(pal), 2));
    painter.setBrush(blend(pal.color(QPalette::Base), accentOf(pal), 0.15));
    painter.drawRoundedRect(QRectF(1, 1, size.width() - 2, size.height() - 2), 6, 6);
    painter.setPen(pal.color(QPalette::Text));
    painter.drawText(QRect(QPoint(0, 0), size), Qt::AlignCenter, name);
    return pixmap;
}

void startDrag(QWidget* source, const QString& name)
{
    auto* mime = new QMimeData;
    mime->setData(LayoutView::kMimeType, name.toUtf8());
    auto* drag = new QDrag(source);
    drag->setMimeData(mime);
    const QPixmap picture = dragPicture(name, source);
    drag->setPixmap(picture);
    drag->setHotSpot(QPoint(picture.width(), picture.height()) / picture.devicePixelRatio() / 2);
    drag->exec(Qt::MoveAction);
}

QString draggedName(const QMimeData* mime)
{
    return mime->hasFormat(LayoutView::kMimeType) ? QString::fromUtf8(mime->data(LayoutView::kMimeType)) : QString();
}

} // namespace

const char* LayoutView::kMimeType = "application/x-leapdesk-screen";

LayoutView::LayoutView(QWidget* parent) :
    QWidget(parent),
    m_Geometry({})
{
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

void LayoutView::setContents(const DesktopGeometry& geometry, const QList<LayoutScreen>& screens)
{
    m_Geometry = geometry;
    m_Screens = screens;
    QStringList names;
    for (const LayoutScreen& screen : screens) {
        if (screen.placed)
            names << tr("%1, reached from the %2 edge").arg(screen.name, ScreenLink::sideLabel(screen.placement.side));
    }
    setAccessibleName(tr("Layout of this computer's monitors"));
    setAccessibleDescription(names.join(QStringLiteral("; ")));
    fit();
    updateGeometry();
    update();
}

void LayoutView::setSelected(const QString& name)
{
    m_Selected = name;
    update();
}

QRectF LayoutView::screenBox(const LayoutScreen& screen) const
{
    if (!screen.placed)
        return QRectF();
    const QRectF b(m_Geometry.bounds());
    const double big = std::max(b.width(), b.height());
    const double gap = big * 0.045;
    const ScreenLink& p = screen.placement;
    if (p.side == Side::Up || p.side == Side::Down) {
        const double x = b.left() + b.width() * p.sourceStart / 100;
        const double w = b.width() * (p.sourceEnd - p.sourceStart) / 100;
        const double h = std::clamp(w / kClientAspect, big * 0.12, big * 0.5);
        return QRectF(x, p.side == Side::Down ? b.bottom() + gap : b.top() - gap - h, w, h);
    }
    const double y = b.top() + b.height() * p.sourceStart / 100;
    const double h = b.height() * (p.sourceEnd - p.sourceStart) / 100;
    const double w = std::clamp(h * kClientAspect, big * 0.12, big * 0.5);
    return QRectF(p.side == Side::Right ? b.right() + gap : b.left() - gap - w, y, w, h);
}

QRectF LayoutView::band(const EdgeZone& zone) const
{
    const QRectF b(m_Geometry.bounds());
    const double big = std::max(b.width(), b.height());
    const double depth = big * 0.16, inset = big * 0.004;
    const QLineF line = m_Geometry.edgeLine(zone.side, zone.from, zone.to);
    switch (zone.side) {
    case Side::Up: return QRectF(line.x1() + inset, line.y1() - depth, line.dx() - 2 * inset, depth);
    case Side::Down: return QRectF(line.x1() + inset, line.y1(), line.dx() - 2 * inset, depth);
    case Side::Left: return QRectF(line.x1() - depth, line.y1() + inset, depth, line.dy() - 2 * inset);
    case Side::Right: return QRectF(line.x1(), line.y1() + inset, depth, line.dy() - 2 * inset);
    }
    return QRectF();
}

QRectF LayoutView::extent() const
{
    const QRectF b(m_Geometry.bounds());
    const double big = std::max(b.width(), b.height());
    QRectF all = b.adjusted(-big * 0.2, -big * 0.2, big * 0.2, big * 0.2);
    for (const LayoutScreen& screen : m_Screens) {
        if (screen.placed) {
            const double pad = big * 0.03;
            all = all.united(screenBox(screen).adjusted(-pad, -pad, pad, pad));
        }
    }
    return all;
}

void LayoutView::fit()
{
    if (m_Geometry.monitors().isEmpty())
        return;
    const QRectF area = QRectF(rect()).adjusted(8, 8, -8, -8);
    const QRectF all = extent();
    m_Scale = std::min(area.width() / all.width(), area.height() / all.height());
    m_Offset = QPointF(area.center().x() - all.center().x() * m_Scale, area.center().y() - all.center().y() * m_Scale);
}

int LayoutView::heightForWidth(int width) const
{
    if (m_Geometry.monitors().isEmpty())
        return 80;
    const QRectF all = extent();
    return std::clamp(static_cast<int>((width - 16) * all.height() / all.width()) + 16, 200, 460);
}

QSize LayoutView::sizeHint() const
{
    return QSize(520, heightForWidth(520));
}

QSize LayoutView::minimumSizeHint() const
{
    return QSize(240, 160);
}

void LayoutView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    fit();
}

QString LayoutView::screenAt(const QPointF& pos) const
{
    const QPointF at = (pos - m_Offset) / m_Scale;
    for (const LayoutScreen& screen : m_Screens) {
        if (screen.placed && screenBox(screen).contains(at))
            return screen.name;
    }
    return QString();
}

int LayoutView::zoneAt(const QPointF& pos) const
{
    const QPointF at = (pos - m_Offset) / m_Scale;
    const auto& zones = m_Geometry.zones();
    for (int i = 0; i < zones.size(); i++) {
        if (!zones[i].combined() && band(zones[i]).contains(at))
            return i;
    }
    return -1;
}

QString LayoutView::zoneTakenBy(int zone, const QString& name) const
{
    const EdgeZone& z = m_Geometry.zones()[zone];
    for (const LayoutScreen& screen : m_Screens) {
        if (!screen.placed || sameName(screen.name, name) || screen.placement.side != z.side)
            continue;
        if (std::min(screen.placement.sourceEnd, z.to) - std::max(screen.placement.sourceStart, z.from) > 0.01)
            return screen.name;
    }
    return QString();
}

void LayoutView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    const QColor text = pal.color(QPalette::WindowText);
    const QColor window = pal.color(QPalette::Window);
    const QColor accent = accentOf(pal);
    const QColor muted = mutedTextColor(pal);

    // the ground the drawing sits on
    painter.setPen(QPen(blend(window, text, 0.12), 1));
    painter.setBrush(blend(window, text, 0.03));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);

    if (m_Geometry.monitors().isEmpty()) {
        painter.setPen(muted);
        painter.drawText(rect(), Qt::AlignCenter, tr("No monitors found"));
        return;
    }

    const QRectF b(m_Geometry.bounds());
    const double big = std::max(b.width(), b.height());
    auto toWidget = [this](const QRectF& r) {
        return QRectF(r.topLeft() * m_Scale + m_Offset, r.size() * m_Scale);
    };
    auto pointToWidget = [this](const QPointF& p) { return p * m_Scale + m_Offset; };
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);

    // the monitors
    for (int i = 0; i < m_Geometry.monitors().size(); i++) {
        const QRect& monitor = m_Geometry.monitors()[i];
        const QRectF box = toWidget(QRectF(monitor)).adjusted(1.5, 1.5, -1.5, -1.5);
        painter.setPen(QPen(blend(window, text, 0.28), 1));
        painter.setBrush(blend(window, text, 0.09));
        painter.drawRoundedRect(box, 3, 3);
        const int line = fontMetrics().height();
        painter.setFont(bold);
        painter.setPen(text);
        painter.drawText(QRectF(box.left(), box.center().y() - line, box.width(), line), Qt::AlignCenter,
                         tr("Monitor %1").arg(i + 1));
        painter.setFont(font());
        painter.setPen(muted);
        painter.drawText(QRectF(box.left(), box.center().y(), box.width(), line), Qt::AlignCenter,
                         QStringLiteral("%1 %2 %3").arg(monitor.width()).arg(QChar(0x00D7)).arg(monitor.height()));
    }

    // where the pointer stops, and where it can leave
    QPen dead(toneColor(Tone::Bad, pal), 2, Qt::DashLine);
    painter.setPen(dead);
    for (const DeadEdge& edge : m_Geometry.deadEdges()) {
        const QPointF a = edge.horizontal ? QPointF(edge.from, edge.fixed) : QPointF(edge.fixed, edge.from);
        const QPointF z = edge.horizontal ? QPointF(edge.to, edge.fixed) : QPointF(edge.fixed, edge.to);
        painter.drawLine(pointToWidget(a), pointToWidget(z));
    }
    painter.setPen(QPen(accent, 3, Qt::SolidLine, Qt::FlatCap));
    for (const EdgeZone& zone : m_Geometry.zones()) {
        if (zone.combined())
            continue;
        const QLineF line = m_Geometry.edgeLine(zone.side, zone.from, zone.to);
        painter.drawLine(pointToWidget(line.p1()), pointToWidget(line.p2()));
    }

    // the screens placed around them, each joined to its stretch of the edge
    const double gap = big * 0.045;
    for (const LayoutScreen& screen : m_Screens) {
        if (!screen.placed)
            continue;
        const QRectF box = screenBox(screen);
        const ScreenLink& p = screen.placement;
        QRectF bridge;
        switch (p.side) {
        case Side::Down: bridge = QRectF(box.left(), b.bottom(), box.width(), gap); break;
        case Side::Up: bridge = QRectF(box.left(), box.bottom(), box.width(), gap); break;
        case Side::Left: bridge = QRectF(box.right(), box.top(), gap, box.height()); break;
        case Side::Right: bridge = QRectF(b.right(), box.top(), gap, box.height()); break;
        }
        QColor joint = accent;
        joint.setAlphaF(sameName(screen.name, m_Dragging) ? 0.1 : 0.28);
        painter.setPen(Qt::NoPen);
        painter.setBrush(joint);
        painter.drawRect(toWidget(bridge));
        const QLineF line = m_Geometry.edgeLine(p.side, p.sourceStart, p.sourceEnd);
        painter.setPen(QPen(accent, 5, Qt::SolidLine, Qt::FlatCap));
        painter.drawLine(pointToWidget(line.p1()), pointToWidget(line.p2()));
    }
    for (const LayoutScreen& screen : m_Screens) {
        if (!screen.placed)
            continue;
        const QRectF box = toWidget(screenBox(screen)).adjusted(1.5, 1.5, -1.5, -1.5);
        const bool selected = sameName(screen.name, m_Selected);
        const bool lifted = sameName(screen.name, m_Dragging);
        painter.setOpacity(lifted ? 0.35 : 1);
        painter.setPen(QPen(selected ? accent : blend(window, text, 0.45), selected ? 2.5 : 1));
        painter.setBrush(blend(pal.color(QPalette::Base), accent, 0.12));
        painter.drawRoundedRect(box, 4, 4);
        const int line = fontMetrics().height();
        painter.setFont(bold);
        painter.setPen(text);
        const QString name = fontMetrics().elidedText(screen.name, Qt::ElideRight, static_cast<int>(box.width()) - 8);
        painter.drawText(QRectF(box.left(), box.center().y() - line, box.width(), line), Qt::AlignCenter, name);
        painter.setFont(font());
        painter.setPen(muted);
        painter.drawText(QRectF(box.left(), box.center().y(), box.width(), line), Qt::AlignCenter, screen.status);
        painter.setOpacity(1);
    }

    // while a screen is dragged, the stretches it can go to
    if (!m_Dragging.isEmpty()) {
        const auto& zones = m_Geometry.zones();
        for (int i = 0; i < zones.size(); i++) {
            const EdgeZone& zone = zones[i];
            if (zone.combined())
                continue;
            const QRectF area = toWidget(band(zone));
            const bool taken = !zoneTakenBy(i, m_Dragging).isEmpty();
            QColor fill = taken ? blend(window, text, 0.25) : accent;
            fill.setAlphaF(taken ? 0.5 : (i == m_HoverZone ? 0.5 : 0.18));
            painter.setPen(QPen(taken ? blend(window, text, 0.4) : accent, 1, i == m_HoverZone ? Qt::SolidLine : Qt::DashLine));
            painter.setBrush(fill);
            painter.drawRoundedRect(area, 4, 4);
            painter.setPen(taken ? muted : text);
            const QString label = zone.side == Side::Left || zone.side == Side::Right
                ? QString::number(zone.monitor)
                : tr("Monitor %1, %2").arg(zone.monitor).arg(ScreenLink::sideLabel(zone.side));
            painter.drawText(area, Qt::AlignCenter, fontMetrics().elidedText(label, Qt::ElideRight, static_cast<int>(area.width())));
        }
    }

    if (hasFocus()) {
        painter.setPen(QPen(text, 1, Qt::DotLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(3.5, 3.5, -3.5, -3.5), 4, 4);
    }
}

void LayoutView::mousePressEvent(QMouseEvent* event)
{
    m_Pressed.clear();
    if (event->button() != Qt::LeftButton)
        return;
    m_PressPos = event->position().toPoint();
    m_Pressed = screenAt(event->position());
    if (!m_Pressed.isEmpty())
        emit screenSelected(m_Pressed);
}

void LayoutView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_Pressed.isEmpty() || !(event->buttons() & Qt::LeftButton) ||
        (event->position().toPoint() - m_PressPos).manhattanLength() < QApplication::startDragDistance())
        return;

    const QString name = m_Pressed;
    m_Pressed.clear();
    m_Dragging = name;
    m_OwnDrag = true;
    update();
    startDrag(this, name);
    m_OwnDrag = false;
    m_Dragging.clear();
    m_HoverZone = -1;
    update();
}

void LayoutView::dragEnterEvent(QDragEnterEvent* event)
{
    const QString name = draggedName(event->mimeData());
    if (name.isEmpty())
        return;
    m_Dragging = name;
    event->acceptProposedAction();
    update();
}

void LayoutView::dragMoveEvent(QDragMoveEvent* event)
{
    const int zone = zoneAt(event->position());
    const bool free = zone >= 0 && zoneTakenBy(zone, m_Dragging).isEmpty();
    if (zone != m_HoverZone) {
        m_HoverZone = free ? zone : -1;
        update();
    }
    if (free)
        event->acceptProposedAction();
    else
        event->ignore();
}

void LayoutView::dragLeaveEvent(QDragLeaveEvent*)
{
    m_HoverZone = -1;
    // a drag that started here keeps its screen faded until it ends
    if (!m_OwnDrag)
        m_Dragging.clear();
    update();
}

void LayoutView::dropEvent(QDropEvent* event)
{
    const QString name = draggedName(event->mimeData());
    const int zone = zoneAt(event->position());
    m_HoverZone = -1;
    m_Dragging.clear();
    update();
    if (name.isEmpty() || zone < 0 || !zoneTakenBy(zone, name).isEmpty())
        return;
    event->acceptProposedAction();
    emit screenDropped(name, zone);
}

ScreenTray::ScreenTray(QWidget* parent) :
    QFrame(parent),
    m_Layout(new QHBoxLayout(this))
{
    setAcceptDrops(true);
    setFrameShape(QFrame::StyledPanel);
    m_Layout->setContentsMargins(8, 6, 8, 6);
    m_Layout->setSpacing(8);
}

void ScreenTray::setScreens(const QStringList& names, const QString& selected, const QString& emptyText)
{
    m_Names = names;
    m_SelectedName = selected;
    m_EmptyText = emptyText;
    if (!m_DragFromHere)
        rebuild();
}

void ScreenTray::rebuild()
{
    while (QLayoutItem* item = m_Layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (m_Names.isEmpty()) {
        auto* empty = new QLabel(m_EmptyText, this);
        empty->setWordWrap(true);
        markMuted(empty);
        m_Layout->addWidget(empty, 1);
        return;
    }
    for (const QString& name : std::as_const(m_Names)) {
        auto* chip = new Chip(name, Tone::Idle, this);
        if (sameName(name, m_SelectedName)) {
            QFont font = chip->font();
            font.setWeight(QFont::DemiBold);
            chip->setFont(font);
        }
        chip->setCursor(Qt::OpenHandCursor);
        chip->setToolTip(tr("Drag it onto a monitor's edge to place it."));
        chip->setProperty("screen", name);
        chip->installEventFilter(this);
        m_Layout->addWidget(chip);
    }
    m_Layout->addStretch();
}

bool ScreenTray::eventFilter(QObject* watched, QEvent* event)
{
    const QString name = watched->property("screen").toString();
    if (name.isEmpty())
        return QFrame::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        m_PressPos = mouse->globalPosition().toPoint();
        // later, since selecting builds the chips again, this one included
        QMetaObject::invokeMethod(this, [this, name]() { emit screenSelected(name); }, Qt::QueuedConnection);
        return true;
    }
    if (event->type() == QEvent::MouseMove) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if ((mouse->buttons() & Qt::LeftButton) &&
            (mouse->globalPosition().toPoint() - m_PressPos).manhattanLength() >= QApplication::startDragDistance()) {
            m_DragFromHere = true;
            startDrag(static_cast<QWidget*>(watched), name);
            m_DragFromHere = false;
            // what changed while the chip was held is shown now that it can go
            QMetaObject::invokeMethod(this, &ScreenTray::rebuild, Qt::QueuedConnection);
        }
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

void ScreenTray::dragEnterEvent(QDragEnterEvent* event)
{
    // only a placed screen, dragged from the drawing, can come off the layout here
    if (!draggedName(event->mimeData()).isEmpty() && qobject_cast<LayoutView*>(event->source()) != nullptr)
        event->acceptProposedAction();
}

void ScreenTray::dropEvent(QDropEvent* event)
{
    const QString name = draggedName(event->mimeData());
    if (name.isEmpty())
        return;
    event->acceptProposedAction();
    emit screenDropped(name);
}

ScreensSection::ScreensSection(QWidget* parent) :
    QWidget(parent),
    m_Geometry({})
{
    build();
}

void ScreensSection::build()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* title = new QLabel(tr("Screens"), this);
    makeSectionTitle(title);
    layout->addWidget(title);
    auto* help = new QLabel(tr("Drag a computer to the monitor edge where the pointer should cross over to it."), this);
    help->setWordWrap(true);
    markMuted(help);
    layout->addWidget(help);

    m_View = new LayoutView(this);
    layout->addWidget(m_View);
    m_Legend = new QLabel(this);
    m_Legend->setTextFormat(Qt::RichText);
    m_Legend->setWordWrap(true);
    markMuted(m_Legend);
    layout->addWidget(m_Legend);

    auto* tray_row = new QHBoxLayout;
    auto* tray_label = new QLabel(tr("Not placed:"), this);
    markMuted(tray_label);
    tray_row->addWidget(tray_label);
    m_Tray = new ScreenTray(this);
    tray_row->addWidget(m_Tray, 1);
    layout->addLayout(tray_row);

    // the selected screen
    m_Inspector = new QFrame(this);
    m_Inspector->setFrameShape(QFrame::StyledPanel);
    auto* inspector = new QGridLayout(m_Inspector);
    inspector->setHorizontalSpacing(10);
    inspector->setVerticalSpacing(8);
    inspector->setColumnStretch(1, 1);
    auto* head = new QHBoxLayout;
    m_InspectorName = new QLabel(m_Inspector);
    m_InspectorName->setTextFormat(Qt::PlainText);
    QFont bold = m_InspectorName->font();
    bold.setWeight(QFont::DemiBold);
    m_InspectorName->setFont(bold);
    m_InspectorStatus = new QLabel(m_Inspector);
    markMuted(m_InspectorStatus);
    head->addWidget(m_InspectorName);
    head->addWidget(m_InspectorStatus);
    head->addStretch();
    inspector->addLayout(head, 0, 0, 1, 2);

    auto* zone_label = new QLabel(tr("&Reached from:"), m_Inspector);
    m_Zone = new QComboBox(m_Inspector);
    m_Zone->setAccessibleName(tr("Reached from"));
    zone_label->setBuddy(m_Zone);
    inspector->addWidget(zone_label, 1, 0);
    inspector->addWidget(m_Zone, 1, 1);

    auto make_percent = [this]() {
        auto* box = new QDoubleSpinBox(m_Inspector);
        box->setRange(0, 100);
        box->setDecimals(2);
        box->setSingleStep(1);
        box->setKeyboardTracking(false);
        connect(box, &QDoubleSpinBox::editingFinished, this, &ScreensSection::stretchEdited);
        return box;
    };
    m_Stretch = new QWidget(m_Inspector);
    auto* stretch = new QGridLayout(m_Stretch);
    stretch->setContentsMargins(0, 0, 0, 0);
    stretch->setHorizontalSpacing(8);
    m_From = make_percent();
    m_To = make_percent();
    m_StretchUnit = new QLabel(m_Stretch);
    markMuted(m_StretchUnit);
    auto* from_label = new QLabel(tr("&From:"), m_Stretch);
    from_label->setBuddy(m_From);
    stretch->addWidget(from_label, 0, 0);
    stretch->addWidget(m_From, 0, 1);
    stretch->addWidget(new QLabel(tr("to"), m_Stretch), 0, 2);
    stretch->addWidget(m_To, 0, 3);
    stretch->addWidget(m_StretchUnit, 0, 4);
    m_TargetFrom = make_percent();
    m_TargetTo = make_percent();
    m_TargetUnit = new QLabel(m_Stretch);
    markMuted(m_TargetUnit);
    auto* target_label = new QLabel(tr("&Enters along:"), m_Stretch);
    target_label->setBuddy(m_TargetFrom);
    stretch->addWidget(target_label, 1, 0);
    stretch->addWidget(m_TargetFrom, 1, 1);
    stretch->addWidget(new QLabel(tr("to"), m_Stretch), 1, 2);
    stretch->addWidget(m_TargetTo, 1, 3);
    stretch->addWidget(m_TargetUnit, 1, 4);
    stretch->setColumnStretch(4, 1);
    inspector->addWidget(m_Stretch, 2, 0, 1, 2);

    m_PlaceText = new QLabel(m_Inspector);
    m_PlaceText->setWordWrap(true);
    markMuted(m_PlaceText);
    inspector->addWidget(m_PlaceText, 3, 0, 1, 2);

    auto* actions = new QHBoxLayout;
    m_ScreenSettings = new QPushButton(tr("Screen &Settings..."), m_Inspector);
    m_ScreenSettings->setAutoDefault(false);
    m_Unpair = new QPushButton(m_Inspector);
    m_Unpair->setAutoDefault(false);
    actions->addWidget(m_ScreenSettings);
    actions->addWidget(m_Unpair);
    actions->addStretch();
    inspector->addLayout(actions, 4, 0, 1, 2);
    layout->addWidget(m_Inspector);

    // the links, with those between other screens
    auto* links_head = new QHBoxLayout;
    auto* links_title = new QLabel(tr("Links"), this);
    links_title->setFont(bold);
    links_head->addWidget(links_title);
    links_head->addStretch();
    m_AddLink = new QPushButton(tr("Add &Link..."), this);
    m_AddLink->setAutoDefault(false);
    links_head->addWidget(m_AddLink);
    layout->addLayout(links_head);
    m_Links = new QWidget(this);
    layout->addWidget(m_Links);

    auto* this_computer = new QHBoxLayout;
    auto* own_settings = new QPushButton(tr("Screen Settings for &This Computer..."), this);
    own_settings->setAutoDefault(false);
    this_computer->addWidget(own_settings);
    this_computer->addStretch();
    layout->addLayout(this_computer);

    connect(m_View, &LayoutView::screenSelected, this, &ScreensSection::select);
    connect(m_View, &LayoutView::screenDropped, this, &ScreensSection::place);
    connect(m_Tray, &ScreenTray::screenSelected, this, &ScreensSection::select);
    connect(m_Tray, &ScreenTray::screenDropped, this, &ScreensSection::unplace);
    connect(m_Zone, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        // later, since placing fills the list again
        const int zone = m_Zone->itemData(index).toInt();
        const QString name = m_Selected;
        QMetaObject::invokeMethod(this, [this, zone, name]() {
            if (zone == -1)
                unplace(name);
            else if (zone >= 0)
                place(name, zone);
        }, Qt::QueuedConnection);
    });
    connect(m_ScreenSettings, &QPushButton::clicked, this, [this]() { editScreenSettings(m_Selected); });
    connect(m_Unpair, &QPushButton::clicked, this, [this]() { emit unpairRequested(m_Selected); });
    connect(m_AddLink, &QPushButton::clicked, this, [this]() { editLink(-1); });
    connect(own_settings, &QPushButton::clicked, this, [this]() {
        if (m_Config != nullptr)
            editScreenSettings(m_Config->serverName());
    });
}

void ScreensSection::setConfig(ServerConfig* config)
{
    m_Config = config;
    refresh();
}

void ScreensSection::setMonitors(const QList<QRect>& monitors)
{
    m_Geometry = DesktopGeometry(monitors);
    refresh();
}

void ScreensSection::setComputers(const QStringList& paired, const QMap<QString, QString>& status)
{
    m_Paired = paired;
    m_Status.clear();
    for (auto it = status.cbegin(); it != status.cend(); ++it)
        m_Status.insert(it.key().toLower(), it.value());
}

bool ScreensSection::isPaired(const QString& name) const
{
    return m_Paired.contains(name, Qt::CaseInsensitive);
}

QList<LayoutScreen> ScreensSection::layoutScreens() const
{
    QList<LayoutScreen> screens;
    if (m_Config == nullptr)
        return screens;

    for (const Screen& screen : std::as_const(*m_Config).screens()) {
        if (sameName(screen.name(), m_Config->serverName()))
            continue;
        LayoutScreen shown;
        shown.name = screen.name();
        shown.status = m_Status.value(screen.name().toLower());
        shown.placed = m_Config->placementOf(screen.name(), &shown.placement);
        screens << shown;
    }
    // a computer paired with but not on the layout waits to be placed
    for (const QString& name : m_Paired) {
        const bool listed = std::any_of(screens.begin(), screens.end(),
                                        [&](const LayoutScreen& screen) { return sameName(screen.name, name); });
        if (!listed && !sameName(name, m_Config->serverName()))
            screens << LayoutScreen{name, m_Status.value(name.toLower()), false, ScreenLink()};
    }
    return screens;
}

void ScreensSection::select(const QString& name)
{
    m_Selected = name;
    refresh();
}

void ScreensSection::refresh()
{
    if (m_Loading)
        return;
    const QList<LayoutScreen> screens = layoutScreens();

    // the screen selected stays so; otherwise the first one placed, or the first one
    const bool still_there = std::any_of(screens.begin(), screens.end(),
                                         [&](const LayoutScreen& screen) { return sameName(screen.name, m_Selected); });
    if (!still_there) {
        m_Selected.clear();
        for (const LayoutScreen& screen : screens) {
            if (screen.placed) {
                m_Selected = screen.name;
                break;
            }
        }
        if (m_Selected.isEmpty() && !screens.isEmpty())
            m_Selected = screens.first().name;
    }

    m_View->setContents(m_Geometry, screens);
    m_View->setSelected(m_Selected);

    QStringList unplaced;
    for (const LayoutScreen& screen : screens) {
        if (!screen.placed)
            unplaced << screen.name;
    }
    m_Tray->setScreens(unplaced, m_Selected,
                       screens.isEmpty() ? tr("Computers you pair with show up here.")
                                         : tr("Every computer has a place. Drop one here to take it off the layout."));

    QStringList sizes;
    for (const QRect& monitor : m_Geometry.monitors())
        sizes << QStringLiteral("%1&nbsp;&#215;&nbsp;%2").arg(monitor.width()).arg(monitor.height());
    const QString accent = accentOf(palette()).name();
    const QString bad = toneColor(Tone::Bad, palette()).name();
    m_Legend->setText(tr("%1 The pointer can leave here &nbsp; %2 The pointer stops here &nbsp; This computer's monitors: %3")
                          .arg(QStringLiteral("<span style=\"color:%1\">&#9644;</span>").arg(accent),
                               QStringLiteral("<span style=\"color:%1\">- - -</span>").arg(bad),
                               sizes.join(QStringLiteral(", "))));

    refreshInspector();
    refreshLinks();
}

QString ScreensSection::explainPlacement(const LayoutScreen& screen) const
{
    if (!screen.placed)
        return tr("Drag it onto a monitor's edge above, or choose one under Reached from.");
    const ScreenLink& p = screen.placement;
    const QString leaves = isWhole(p.sourceStart, p.sourceEnd)
        ? tr("all of") : tr("%1 to %2 % of").arg(percent(p.sourceStart), percent(p.sourceEnd));
    const QString enters = isWhole(p.targetStart, p.targetEnd)
        ? tr("all of") : tr("%1 to %2 % of").arg(percent(p.targetStart), percent(p.targetEnd));
    QString text = tr("The pointer leaves %1 through %2 its %3 edge, and enters %4 through %5 its %6 edge, which leads back.")
        .arg(m_Config->serverName(), leaves, ScreenLink::sideLabel(p.side), screen.name, enters,
             ScreenLink::sideLabel(ScreenLink::opposite(p.side)));
    if (!m_Geometry.monitors().isEmpty() && !m_Geometry.reaches(p.side, p.sourceStart, p.sourceEnd))
        text += " " + tr("Part of that stretch has no monitor beside it, so the pointer can't get there.");
    return text;
}

void ScreensSection::refreshInspector()
{
    const QList<LayoutScreen> screens = layoutScreens();
    auto it = std::find_if(screens.begin(), screens.end(),
                           [&](const LayoutScreen& screen) { return sameName(screen.name, m_Selected); });
    m_Inspector->setVisible(it != screens.end());
    if (it == screens.end())
        return;
    const LayoutScreen& screen = *it;

    m_InspectorName->setText(screen.name);
    m_InspectorStatus->setText(screen.status);

    // the zones, with those another screen has taken shown but not offered
    QSignalBlocker block(m_Zone);
    m_Zone->clear();
    m_Zone->addItem(tr("Not placed"), -1);
    int current = 0;
    const int zone_of_placement = screen.placed
        ? m_Geometry.zoneFor(screen.placement.side, screen.placement.sourceStart, screen.placement.sourceEnd) : -1;
    const auto& zones = m_Geometry.zones();
    for (int i = 0; i < zones.size(); i++) {
        const EdgeZone& zone = zones[i];
        const QString taken = m_Config->stretchLeadsTo(zone.side, zone.from, zone.to, screen.name);
        m_Zone->addItem(taken.isEmpty() ? zoneWords(zone) : tr("%1, leads to %2").arg(zoneWords(zone), taken), i);
        if (!taken.isEmpty()) {
            if (auto* model = qobject_cast<QStandardItemModel*>(m_Zone->model()))
                model->item(m_Zone->count() - 1)->setEnabled(false);
        }
        if (i == zone_of_placement)
            current = m_Zone->count() - 1;
    }
    if (screen.placed && zone_of_placement < 0) {
        // the monitors changed since it was placed, say
        m_Zone->addItem(tr("Another stretch of the %1 edge").arg(ScreenLink::sideLabel(screen.placement.side)), -2);
        current = m_Zone->count() - 1;
    }
    m_Zone->setCurrentIndex(current);

    m_Stretch->setVisible(screen.placed);
    if (screen.placed) {
        const ScreenLink& p = screen.placement;
        for (auto [box, value] : {std::pair(m_From, p.sourceStart), std::pair(m_To, p.sourceEnd),
                                  std::pair(m_TargetFrom, p.targetStart), std::pair(m_TargetTo, p.targetEnd)}) {
            QSignalBlocker block_box(box);
            box->setValue(value);
        }
        m_StretchUnit->setText(tr("% along the %1 edge").arg(ScreenLink::sideLabel(p.side)));
        m_TargetUnit->setText(tr("% of its %1 edge").arg(ScreenLink::sideLabel(ScreenLink::opposite(p.side))));
    }
    m_PlaceText->setText(explainPlacement(screen));

    m_ScreenSettings->setEnabled(m_Config->hasScreen(screen.name));
    m_Unpair->setVisible(isPaired(screen.name));
    m_Unpair->setText(tr("Unpair %1...").arg(screen.name));
}

void ScreensSection::refreshLinks()
{
    for (QWidget* old : m_Links->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        old->hide();
        old->deleteLater();
    }
    delete m_Links->layout();
    auto* grid = new QGridLayout(m_Links);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    if (m_Config == nullptr)
        return;

    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const QStringList heads = {tr("Leaving"), tr("Enters")};
    for (int column = 0; column < heads.size(); column++) {
        auto* head = new QLabel(heads[column], m_Links);
        head->setFont(bold);
        markMuted(head);
        grid->addWidget(head, 0, column);
    }

    const auto& links = std::as_const(*m_Config).links();
    auto leaving = [](const ScreenLink& link) {
        return tr("%1, %2 edge, %3").arg(link.source, ScreenLink::sideLabel(link.side), stretchWords(link.sourceStart, link.sourceEnd));
    };
    auto entering = [](const ScreenLink& link) {
        return tr("%1, %2 edge, %3").arg(link.target, ScreenLink::sideLabel(ScreenLink::opposite(link.side)),
                                         stretchWords(link.targetStart, link.targetEnd));
    };
    int row = 1;
    auto add_row = [&](const ScreenLink& link, QWidget* action) {
        auto* from = new QLabel(leaving(link), m_Links);
        from->setWordWrap(true);
        auto* to = new QLabel(entering(link), m_Links);
        to->setWordWrap(true);
        grid->addWidget(from, row, 0);
        grid->addWidget(to, row, 1);
        if (action != nullptr)
            grid->addWidget(action, row, 2);
        row++;
    };

    // each placed screen's link and its way back, then the rest
    std::vector<bool> shown(links.size(), false);
    const QString server = m_Config->serverName();
    for (const LayoutScreen& screen : layoutScreens()) {
        if (!screen.placed)
            continue;
        for (size_t i = 0; i < links.size(); i++) {
            if (!shown[i] && sameName(links[i].source, server) && sameName(links[i].target, screen.name)) {
                shown[i] = true;
                auto* edit = new QPushButton(tr("Select"), m_Links);
                edit->setAccessibleName(tr("Select %1").arg(screen.name));
                edit->setAutoDefault(false);
                const QString name = screen.name;
                connect(edit, &QPushButton::clicked, this, [this, name]() { select(name); });
                add_row(links[i], edit);
                break;
            }
        }
        for (size_t i = 0; i < links.size(); i++) {
            if (!shown[i] && sameName(links[i].source, screen.name) && sameName(links[i].target, server)) {
                shown[i] = true;
                auto* back = new QLabel(tr("The way back"), m_Links);
                markMuted(back);
                add_row(links[i], back);
                break;
            }
        }
    }
    for (size_t i = 0; i < links.size(); i++) {
        if (shown[i])
            continue;
        auto* buttons = new QWidget(m_Links);
        auto* buttons_layout = new QHBoxLayout(buttons);
        buttons_layout->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QPushButton(tr("Edit..."), buttons);
        edit->setAutoDefault(false);
        const int index = static_cast<int>(i);
        connect(edit, &QPushButton::clicked, this, [this, index]() { editLink(index); });
        auto* remove = new QPushButton(tr("Remove"), buttons);
        remove->setAutoDefault(false);
        connect(remove, &QPushButton::clicked, this, [this, index]() { removeLink(index); });
        buttons_layout->addWidget(edit);
        buttons_layout->addWidget(remove);
        add_row(links[i], buttons);
    }
    if (row == 1) {
        auto* none = new QLabel(tr("No links yet."), m_Links);
        markMuted(none);
        grid->addWidget(none, 1, 0, 1, 3);
    }
    m_AddLink->setEnabled(m_Config->numScreens() >= 2);
}

void ScreensSection::place(const QString& name, int zone)
{
    if (m_Config == nullptr || name.isEmpty() || zone < 0 || zone >= m_Geometry.zones().size())
        return;
    const EdgeZone& z = m_Geometry.zones()[zone];

    ScreenLink placement;
    if (!m_Config->placementOf(name, &placement)) {
        placement.targetStart = 0;
        placement.targetEnd = 100;
    }
    placement.side = z.side;
    placement.sourceStart = rounded(z.from);
    placement.sourceEnd = rounded(z.to);

    QString blocked_by;
    if (!m_Config->place(name, placement, &blocked_by)) {
        QMessageBox::information(this, tr("Screens"), tr("That stretch already leads to %1.").arg(blocked_by));
        return;
    }
    m_Selected = name;
    refresh();
    emit changed();
}

void ScreensSection::unplace(const QString& name)
{
    if (m_Config == nullptr || name.isEmpty())
        return;
    ScreenLink placement;
    if (!m_Config->placementOf(name, &placement))
        return;
    m_Config->unplace(name);
    m_Selected = name;
    refresh();
    emit changed();
}

void ScreensSection::stretchEdited()
{
    ScreenLink placement;
    if (m_Config == nullptr || !m_Config->placementOf(m_Selected, &placement))
        return;

    // within the monitor's edge it was put on, and at least 1 % long
    double low = 0, high = 100;
    const int zone = m_Geometry.zoneFor(placement.side, placement.sourceStart, placement.sourceEnd);
    if (zone >= 0) {
        low = rounded(m_Geometry.zones()[zone].from);
        high = rounded(m_Geometry.zones()[zone].to);
    }
    ScreenLink edited = placement;
    edited.sourceStart = std::clamp(m_From->value(), low, high);
    edited.sourceEnd = std::clamp(m_To->value(), low, high);
    edited.targetStart = m_TargetFrom->value();
    edited.targetEnd = m_TargetTo->value();
    if (edited == placement)
        return;
    if (edited.sourceEnd - edited.sourceStart < 1 || edited.targetEnd - edited.targetStart < 1) {
        QMessageBox::information(this, tr("Screens"), tr("A stretch has to be at least 1 % long."));
        refreshInspector();
        return;
    }

    QString blocked_by;
    if (!m_Config->place(m_Selected, edited, &blocked_by)) {
        QMessageBox::information(this, tr("Screens"),
                                 tr("That overlaps the stretch leading to %1.").arg(blocked_by));
        refreshInspector();
        return;
    }
    refresh();
    emit changed();
}

void ScreensSection::editScreenSettings(const QString& name)
{
    if (m_Config == nullptr)
        return;
    if (sameName(name, m_Config->serverName()))
        m_Config->ensureServerScreen(QString());

    auto& screens = m_Config->screens();
    auto it = std::find_if(screens.begin(), screens.end(),
                           [&](const Screen& screen) { return sameName(screen.name(), name); });
    if (it == screens.end())
        return;

    Screen edited = *it;
    ScreenSettingsDialog dialog(this, &edited, false);
    if (dialog.exec() != QDialog::Accepted)
        return;
    *it = edited;
    refresh();
    emit changed();
}

bool ScreensSection::storeLink(const ScreenLink& link, int index)
{
    auto& links = m_Config->links();
    for (int i = 0; i < static_cast<int>(links.size()); i++) {
        const ScreenLink& other = links[i];
        if (i == index || !sameName(other.source, link.source) || other.side != link.side)
            continue;
        if (link.sourceStart < other.sourceEnd && other.sourceStart < link.sourceEnd) {
            QMessageBox::warning(this, tr("Links overlap"),
                                 tr("The %1 edge of %2 already leads to %3 along %4. A stretch of an edge can "
                                    "lead to only one place; shorten one of the two links.")
                                     .arg(ScreenLink::sideLabel(link.side), link.source, other.target,
                                          stretchWords(other.sourceStart, other.sourceEnd)));
            return false;
        }
    }
    if (index < 0)
        links.push_back(link);
    else
        links[index] = link;
    return true;
}

void ScreensSection::editLink(int index)
{
    if (m_Config == nullptr)
        return;
    QStringList names;
    for (const Screen& screen : std::as_const(*m_Config).screens())
        names << screen.name();
    if (names.size() < 2)
        return;

    auto& links = m_Config->links();
    const bool add = index < 0;
    if (!add && index >= static_cast<int>(links.size()))
        return;

    ScreenLink link;
    int back_index = -1;
    if (add) {
        link.source = names.first();
        link.target = names.at(1);
    } else {
        link = links[index];
        const ScreenLink back = link.reversed();
        auto found = std::find(links.begin(), links.end(), back);
        if (found != links.end() && found - links.begin() != index)
            back_index = static_cast<int>(found - links.begin());
    }

    LinkDialog dialog(this, names, link, add, back_index >= 0);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const ScreenLink edited = dialog.link();
    if (!storeLink(edited, index))
        return;
    if (dialog.includeLinkBack())
        storeLink(edited.reversed(), back_index);
    refresh();
    emit changed();
}

void ScreensSection::removeLink(int index)
{
    if (m_Config == nullptr)
        return;
    auto& links = m_Config->links();
    if (index < 0 || index >= static_cast<int>(links.size()))
        return;

    const ScreenLink removed = links[index];
    const ScreenLink back = removed.reversed();
    auto found = std::find(links.begin(), links.end(), back);
    bool remove_back = false;
    if (found != links.end() && found - links.begin() != index) {
        const int answer = QMessageBox::question(
            this, tr("Remove link"),
            tr("Also remove the way back, from the %1 edge of %2 to %3?")
                .arg(ScreenLink::sideLabel(back.side), back.source, back.target),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
            return;
        remove_back = answer == QMessageBox::Yes;
    }

    links.erase(links.begin() + index);
    if (remove_back)
        links.erase(std::find(links.begin(), links.end(), back));
    refresh();
    emit changed();
}

void ScreensSection::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        delete layout();
        qDeleteAll(findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly));
        build();
        refresh();
    }
    QWidget::changeEvent(event);
}
