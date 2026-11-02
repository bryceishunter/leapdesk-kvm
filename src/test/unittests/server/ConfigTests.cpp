/*
 * Leapdesk KVM -- mouse and keyboard sharing utility
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

#include "server/Config.h"

#include <gtest/gtest.h>
#include <sstream>

namespace inputleap {

namespace {

const char* const kPartialEdgeConfig =
    "section: screens\n"
    "    desk:\n"
    "    studio:\n"
    "end\n"
    "section: links\n"
    "    desk:\n"
    "        down(42.86,100) = studio\n"
    "    studio:\n"
    "        up = desk(42.86,100)\n"
    "end\n";

Config read_config(const std::string& text)
{
    Config config;
    std::istringstream in(text);
    in >> config;
    return config;
}

std::string write_config(const Config& config)
{
    std::ostringstream out;
    out << config;
    return out.str();
}

} // namespace

TEST(ConfigTests, formatInterval_fullEdge_isEmpty)
{
    EXPECT_EQ(Config::formatInterval(Config::Interval(0.0f, 1.0f)), "");
}

TEST(ConfigTests, formatInterval_wholePercents_haveNoDecimals)
{
    EXPECT_EQ(Config::formatInterval(Config::Interval(0.0f, 0.5f)), "(0,50)");
}

TEST(ConfigTests, formatInterval_fractionalPercents_keepTwoDecimals)
{
    EXPECT_EQ(Config::formatInterval(Config::Interval(0.4286f, 1.0f)), "(42.86,100)");
    EXPECT_EQ(Config::formatInterval(Config::Interval(0.125f, 0.5f)), "(12.5,50)");
    EXPECT_EQ(Config::formatInterval(Config::Interval(0.0005f, 0.0105f)), "(0.05,1.05)");
}

TEST(ConfigTests, write_partialEdgeLinks_keepTheirIntervals)
{
    std::string text = write_config(read_config(kPartialEdgeConfig));

    EXPECT_NE(text.find("down(42.86,100) = studio"), std::string::npos) << text;
    EXPECT_NE(text.find("up = desk(42.86,100)"), std::string::npos) << text;
}

TEST(ConfigTests, addOption_existingOption_replacesItsValue)
{
    Config config = read_config(kPartialEdgeConfig);
    config.addOption("", kOptionClipboardSharing, 1);

    config.addOption("", kOptionClipboardSharing, 0);

    EXPECT_EQ(config.getOptions("")->at(kOptionClipboardSharing), 0);
}

TEST(ConfigTests, readWrite_clipboardSharingSize_keepsTheNumber)
{
    Config original = read_config(std::string(kPartialEdgeConfig) +
                                  "section: options\n"
                                  "    clipboardSharing = true\n"
                                  "    clipboardSharingSize = 97000000\n"
                                  "end\n");

    std::string text = write_config(original);

    EXPECT_NE(text.find("clipboardSharingSize = 97000000"), std::string::npos) << text;
    EXPECT_EQ(original, read_config(text));
}

TEST(ConfigTests, removeScreen_withOtherScreens_removesItAndItsLinks)
{
    Config config = read_config(kPartialEdgeConfig);
    config.addAlias("studio", "surface");

    config.removeScreen("studio");

    EXPECT_TRUE(config.isScreen("desk"));
    EXPECT_FALSE(config.isScreen("studio"));
    EXPECT_FALSE(config.isScreen("surface"));
    EXPECT_FALSE(config.hasNeighbor("desk", kBottom));
}

TEST(ConfigTests, readWrite_partialEdgeConfig_roundTripsUnchanged)
{
    Config original = read_config(kPartialEdgeConfig);
    Config reread = read_config(write_config(original));

    EXPECT_EQ(original, reread);
}

} // namespace inputleap
