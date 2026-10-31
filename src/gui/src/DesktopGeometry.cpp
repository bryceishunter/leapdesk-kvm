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

#include "DesktopGeometry.h"

#include <algorithm>
#include <cmath>

namespace {

using Side = ScreenLink::Side;
using Segment = std::pair<int, int>;

// what's left of segments once cut is taken out of them
QList<Segment> subtract(const QList<Segment>& segments, const Segment& cut)
{
    QList<Segment> left;
    for (const Segment& segment : segments) {
        if (cut.second <= segment.first || cut.first >= segment.second) {
            left << segment;
            continue;
        }
        if (cut.first > segment.first)
            left << Segment(segment.first, cut.first);
        if (cut.second < segment.second)
            left << Segment(cut.second, segment.second);
    }
    return left;
}

// QRect's right() and bottom() are one short of the edge
int rightOf(const QRect& r) { return r.x() + r.width(); }
int bottomOf(const QRect& r) { return r.y() + r.height(); }

} // namespace

DesktopGeometry::DesktopGeometry(QList<QRect> monitors) :
    m_Monitors(std::move(monitors))
{
    std::sort(m_Monitors.begin(), m_Monitors.end(), [](const QRect& a, const QRect& b) {
        return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y();
    });
    if (m_Monitors.isEmpty())
        return;

    int x0 = m_Monitors.first().x(), y0 = m_Monitors.first().y();
    int x1 = rightOf(m_Monitors.first()), y1 = bottomOf(m_Monitors.first());
    for (const QRect& monitor : std::as_const(m_Monitors)) {
        x0 = std::min(x0, monitor.x());
        y0 = std::min(y0, monitor.y());
        x1 = std::max(x1, rightOf(monitor));
        y1 = std::max(y1, bottomOf(monitor));
    }
    m_Bounds = QRect(x0, y0, x1 - x0, y1 - y0);
    const double width = m_Bounds.width(), height = m_Bounds.height();

    for (int i = 0; i < m_Monitors.size(); i++) {
        const QRect& m = m_Monitors[i];
        struct Edge { Side side; int fixed; int from; int to; bool horizontal; bool outer; };
        const Edge edges[] = {
            {Side::Up, m.y(), m.x(), rightOf(m), true, m.y() == y0},
            {Side::Down, bottomOf(m), m.x(), rightOf(m), true, bottomOf(m) == y1},
            {Side::Left, m.x(), m.y(), bottomOf(m), false, m.x() == x0},
            {Side::Right, rightOf(m), m.y(), bottomOf(m), false, rightOf(m) == x1},
        };
        for (const Edge& edge : edges) {
            if (edge.outer) {
                const double origin = edge.horizontal ? x0 : y0;
                const double length = edge.horizontal ? width : height;
                m_Zones << EdgeZone{edge.side, i + 1, (edge.from - origin) / length * 100,
                                    (edge.to - origin) / length * 100};
                continue;
            }
            // inside the box, the pointer stops wherever no other monitor meets this edge
            QList<Segment> open = {Segment(edge.from, edge.to)};
            for (const QRect& other : std::as_const(m_Monitors)) {
                if (&other == &m)
                    continue;
                const bool meets = (edge.side == Side::Down && other.y() == edge.fixed) ||
                                   (edge.side == Side::Up && bottomOf(other) == edge.fixed) ||
                                   (edge.side == Side::Right && other.x() == edge.fixed) ||
                                   (edge.side == Side::Left && rightOf(other) == edge.fixed);
                if (meets) {
                    open = subtract(open, edge.horizontal ? Segment(other.x(), rightOf(other))
                                                          : Segment(other.y(), bottomOf(other)));
                }
            }
            for (const Segment& segment : open) {
                if (segment.second > segment.first)
                    m_DeadEdges << DeadEdge{edge.horizontal, edge.fixed, segment.first, segment.second};
            }
        }
    }

    // where every monitor's stretch of an edge follows on from the last, one zone covers them all
    for (Side side : {Side::Up, Side::Down, Side::Left, Side::Right}) {
        QList<EdgeZone> along;
        for (const EdgeZone& zone : std::as_const(m_Zones)) {
            if (zone.side == side)
                along << zone;
        }
        std::sort(along.begin(), along.end(), [](const EdgeZone& a, const EdgeZone& b) { return a.from < b.from; });
        bool joined = along.size() > 1;
        for (int i = 1; i < along.size(); i++)
            joined = joined && std::abs(along[i - 1].to - along[i].from) < 1e-6;
        if (joined)
            m_Zones << EdgeZone{side, 0, along.first().from, along.last().to};
    }
}

int DesktopGeometry::zoneFor(ScreenLink::Side side, double from, double to) const
{
    int combined = -1;
    for (int i = 0; i < m_Zones.size(); i++) {
        const EdgeZone& zone = m_Zones[i];
        if (zone.side != side || !zone.contains(from, to))
            continue;
        if (!zone.combined())
            return i;
        combined = i;
    }
    return combined;
}

bool DesktopGeometry::reaches(ScreenLink::Side side, double from, double to) const
{
    QList<EdgeZone> along;
    for (const EdgeZone& zone : m_Zones) {
        if (zone.side == side && !zone.combined())
            along << zone;
    }
    std::sort(along.begin(), along.end(), [](const EdgeZone& a, const EdgeZone& b) { return a.from < b.from; });

    // walk along the stretch over the monitors' zones, which must leave no gap
    double reached = from;
    for (const EdgeZone& zone : along) {
        if (zone.from > reached + 0.01)
            break;
        reached = std::max(reached, zone.to);
    }
    return reached >= to - 0.01;
}

QLineF DesktopGeometry::edgeLine(ScreenLink::Side side, double from, double to) const
{
    const QRectF b(m_Bounds);
    switch (side) {
    case Side::Up:
    case Side::Down: {
        const double y = side == Side::Up ? b.top() : b.top() + b.height();
        return QLineF(b.left() + b.width() * from / 100, y, b.left() + b.width() * to / 100, y);
    }
    case Side::Left:
    case Side::Right: {
        const double x = side == Side::Left ? b.left() : b.left() + b.width();
        return QLineF(x, b.top() + b.height() * from / 100, x, b.top() + b.height() * to / 100);
    }
    }
    return QLineF();
}
