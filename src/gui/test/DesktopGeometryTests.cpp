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

#include "../src/DesktopGeometry.h"

#include <gtest/gtest.h>

namespace {

using Side = ScreenLink::Side;

// a 1920 x 1080 monitor on the left, and a taller 2560 x 1440 one to its right, tops aligned
DesktopGeometry twoMonitors()
{
    return DesktopGeometry({QRect(1920, 0, 2560, 1440), QRect(0, 0, 1920, 1080)});
}

const EdgeZone* find(const DesktopGeometry& geometry, Side side, int monitor)
{
    for (const EdgeZone& zone : geometry.zones()) {
        if (zone.side == side && zone.monitor == monitor)
            return &zone;
    }
    return nullptr;
}

} // namespace

TEST(DesktopGeometryTests, numbersMonitorsLeftToRight)
{
    auto geometry = twoMonitors();
    EXPECT_EQ(geometry.monitors()[0], QRect(0, 0, 1920, 1080));
    EXPECT_EQ(geometry.bounds(), QRect(0, 0, 4480, 1440));
}

TEST(DesktopGeometryTests, aMonitorsOuterEdgeIsAZone)
{
    auto geometry = twoMonitors();

    const EdgeZone* down = find(geometry, Side::Down, 2);
    ASSERT_NE(down, nullptr);
    EXPECT_NEAR(down->from, 42.857, 0.001);
    EXPECT_DOUBLE_EQ(down->to, 100);

    const EdgeZone* left = find(geometry, Side::Left, 1);
    ASSERT_NE(left, nullptr);
    EXPECT_DOUBLE_EQ(left->from, 0);
    EXPECT_DOUBLE_EQ(left->to, 75);

    // the shorter monitor's bottom is inside the box
    EXPECT_EQ(find(geometry, Side::Down, 1), nullptr);
    EXPECT_EQ(find(geometry, Side::Right, 1), nullptr);
}

TEST(DesktopGeometryTests, edgesThatLineUpAlsoMakeOneZone)
{
    auto geometry = twoMonitors();

    const EdgeZone* top = find(geometry, Side::Up, 0);
    ASSERT_NE(top, nullptr);
    EXPECT_DOUBLE_EQ(top->from, 0);
    EXPECT_DOUBLE_EQ(top->to, 100);
    EXPECT_EQ(find(geometry, Side::Down, 0), nullptr);
}

TEST(DesktopGeometryTests, thePointerStopsWhereNoMonitorIsBeyond)
{
    auto geometry = twoMonitors();

    ASSERT_EQ(geometry.deadEdges().size(), 2);
    // under the shorter monitor
    EXPECT_TRUE(geometry.deadEdges()[0].horizontal);
    EXPECT_EQ(geometry.deadEdges()[0].fixed, 1080);
    EXPECT_EQ(geometry.deadEdges()[0].from, 0);
    EXPECT_EQ(geometry.deadEdges()[0].to, 1920);
    // the taller monitor's left edge below it
    EXPECT_FALSE(geometry.deadEdges()[1].horizontal);
    EXPECT_EQ(geometry.deadEdges()[1].fixed, 1920);
    EXPECT_EQ(geometry.deadEdges()[1].from, 1080);
    EXPECT_EQ(geometry.deadEdges()[1].to, 1440);
}

TEST(DesktopGeometryTests, findsTheZoneAStretchIsIn)
{
    auto geometry = twoMonitors();

    int index = geometry.zoneFor(Side::Down, 42.86, 100);
    ASSERT_GE(index, 0);
    EXPECT_EQ(geometry.zones()[index].monitor, 2);

    // across both monitors only the zone along all of them holds it
    index = geometry.zoneFor(Side::Up, 20, 80);
    ASSERT_GE(index, 0);
    EXPECT_TRUE(geometry.zones()[index].combined());

    // nothing but the shorter monitor's inner edge is there
    EXPECT_EQ(geometry.zoneFor(Side::Down, 0, 40), -1);
}

TEST(DesktopGeometryTests, knowsWhereThePointerCantReach)
{
    auto geometry = twoMonitors();

    EXPECT_TRUE(geometry.reaches(Side::Down, 42.86, 100));
    EXPECT_TRUE(geometry.reaches(Side::Up, 0, 100));
    // under the shorter monitor there's nothing to leave from
    EXPECT_FALSE(geometry.reaches(Side::Down, 20, 80));
    EXPECT_FALSE(geometry.reaches(Side::Left, 0, 100));
}

TEST(DesktopGeometryTests, oneMonitorHasAZoneOnEachSide)
{
    DesktopGeometry geometry({QRect(0, 0, 2400, 1600)});

    EXPECT_EQ(geometry.zones().size(), 4);
    EXPECT_TRUE(geometry.deadEdges().isEmpty());
    EXPECT_EQ(geometry.edgeLine(Side::Down, 20, 80), QLineF(480, 1600, 1920, 1600));
}
