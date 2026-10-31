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

#include "ScreenLink.h"

#include <QLineF>
#include <QList>
#include <QRect>

// A stretch of the desktop's outer edge along one monitor, or along all of them where their
// edges line up, where the pointer can leave for another screen.
struct EdgeZone
{
    ScreenLink::Side side = ScreenLink::Side::Right;
    int monitor = 0;    // numbered from 1, or 0 for a zone across all the monitors
    double from = 0;    // in percent of the desktop's edge, as links measure it
    double to = 100;

    bool combined() const { return monitor == 0; }
    bool contains(double start, double end) const { return start >= from - 0.01 && end <= to + 0.01; }
};

// A stretch of a monitor's edge inside the desktop with no monitor beyond it, where the pointer
// stops, in desktop pixels.
struct DeadEdge
{
    bool horizontal = true;
    int fixed = 0;
    int from = 0;
    int to = 0;
};

// The server's monitors as one desktop. The server treats them as one screen and switches only
// at the box around them, so a link is a stretch of that box's edge; where a monitor's edge
// stops short of the box, the pointer stops there.
class DesktopGeometry
{
public:
    // monitors in desktop pixels; they're numbered left to right, then top to bottom
    explicit DesktopGeometry(QList<QRect> monitors);

    const QList<QRect>& monitors() const { return m_Monitors; }
    const QRect& bounds() const { return m_Bounds; }
    // each monitor's zones, then those across all of them
    const QList<EdgeZone>& zones() const { return m_Zones; }
    const QList<DeadEdge>& deadEdges() const { return m_DeadEdges; }

    // the zone a stretch fits in, a monitor's own before one across all of them, or -1
    int zoneFor(ScreenLink::Side side, double from, double to) const;
    // whether the pointer can reach all of a stretch, which it can't where no monitor is beside it
    bool reaches(ScreenLink::Side side, double from, double to) const;
    // the stretch of the box's edge, in desktop pixels
    QLineF edgeLine(ScreenLink::Side side, double from, double to) const;

private:
    QList<QRect> m_Monitors;
    QRect m_Bounds;
    QList<EdgeZone> m_Zones;
    QList<DeadEdge> m_DeadEdges;
};
