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

#include "common/DataDirectories.h"
#include "io/FileChangeWatch.h"
#include "net/FingerprintDatabase.h"
#include "net/PairedComputers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

namespace inputleap {

namespace {

const FingerprintData kDesk{"sha256", {1, 2, 3, 0xab}};
const FingerprintData kStudio{"sha256", {4, 5, 6, 0xcd}};

// a profile folder of its own, with the one the tests found put back afterwards
class PairedComputersFiles : public ::testing::Test {
protected:
    void SetUp() override
    {
        original_profile_ = DataDirectories::profile();
        const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
        folder_ = fs::temp_directory_path() / (std::string("leapdesk-") + test->name());
        fs::remove_all(folder_);
        DataDirectories::profile(folder_);
        fs::create_directories(DataDirectories::ssl_fingerprints_path());
        trust_path_ = DataDirectories::trusted_clients_ssl_fingerprints_path();
    }

    void TearDown() override
    {
        DataDirectories::profile(original_profile_);
        fs::remove_all(folder_);
    }

    void write_trust(std::initializer_list<FingerprintData> fingerprints)
    {
        FingerprintDatabase db;
        for (const auto& fingerprint : fingerprints) {
            db.add_trusted(fingerprint);
        }
        ASSERT_TRUE(db.write(trust_path_));
    }

    fs::path original_profile_;
    fs::path folder_;
    fs::path trust_path_;
};

} // namespace

TEST(PairedNames, readsAndWritesNamesByFingerprint)
{
    PairedNames names;
    names.set_name(kDesk, "desk");
    names.set_name(kStudio, "studio");
    std::ostringstream written;
    names.write_stream(written);

    PairedNames read;
    std::istringstream text(written.str());
    read.read_stream(text);

    EXPECT_EQ(read.name_of(kDesk), "desk");
    EXPECT_EQ(read.name_of(kStudio), "studio");
}

TEST(PairedNames, skipsLinesItCantUse)
{
    std::istringstream text("\nno-fingerprint name\nv2:sha256:0102\nv2:sha256:zz name\nv2:sha256:010203ab desk\n");
    PairedNames names;
    names.read_stream(text);

    EXPECT_EQ(names.name_of(kDesk), "desk");
    EXPECT_EQ(names.name_of(FingerprintData{"sha256", {1, 2}}), "");
}

TEST(PairedNames, aNewNameReplacesTheOldOne)
{
    PairedNames names;
    names.set_name(kDesk, "desk");
    names.set_name(kDesk, "office");
    names.forget(kStudio);

    EXPECT_EQ(names.name_of(kDesk), "office");

    names.forget(kDesk);
    EXPECT_EQ(names.name_of(kDesk), "");
}

TEST(PeerFingerprint, isInvalidWithoutASocketThatChecksCertificates)
{
    EXPECT_FALSE(peer_fingerprint(nullptr).valid());
}

TEST_F(PairedComputersFiles, forgettingStopsTrustingOnlyThatComputer)
{
    write_trust({kDesk, kStudio});
    remember_computer_name(kDesk, "desk");
    remember_computer_name(kStudio, "studio");

    EXPECT_TRUE(forget_computer(trust_path_, kDesk));

    FingerprintDatabase db;
    ASSERT_TRUE(db.read(trust_path_));
    EXPECT_FALSE(db.is_trusted(kDesk));
    EXPECT_TRUE(db.is_trusted(kStudio));

    PairedNames names;
    names.read(DataDirectories::paired_names_path());
    EXPECT_EQ(names.name_of(kDesk), "");
    EXPECT_EQ(names.name_of(kStudio), "studio");
}

TEST_F(PairedComputersFiles, forgettingAComputerNotTrustedChangesNothing)
{
    write_trust({kStudio});

    EXPECT_TRUE(forget_computer(trust_path_, kDesk));

    FingerprintDatabase db;
    ASSERT_TRUE(db.read(trust_path_));
    EXPECT_TRUE(db.is_trusted(kStudio));
}

TEST_F(PairedComputersFiles, forgettingWithoutTheListFails)
{
    EXPECT_FALSE(forget_computer(trust_path_, kDesk));
    EXPECT_FALSE(fs::exists(trust_path_));
}

TEST_F(PairedComputersFiles, aListThatCantBeReadIsToldApartFromAnEmptyOne)
{
    FingerprintDatabase db;
    EXPECT_FALSE(db.read(trust_path_));

    write_trust({});
    EXPECT_TRUE(db.read(trust_path_));
    EXPECT_TRUE(db.fingerprints().empty());
}

TEST_F(PairedComputersFiles, replacingAFileLeavesNothingElseBehind)
{
    write_trust({kDesk});
    write_trust({kStudio});

    int files = 0;
    for (const auto& entry : fs::directory_iterator(trust_path_.parent_path())) {
        (void) entry;
        ++files;
    }
    EXPECT_EQ(files, 1);
}

TEST_F(PairedComputersFiles, aFileHeldOpenBrieflyIsStillReplaced)
{
    write_trust({kDesk});

    std::thread reader;
    {
        auto held = std::make_shared<std::ifstream>();
        open_utf8_path(*held, trust_path_, std::ios_base::in);
        ASSERT_TRUE(held->is_open());
        reader = std::thread([held]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            held->close();
        });
    }
    write_trust({kStudio});
    reader.join();

    FingerprintDatabase db;
    ASSERT_TRUE(db.read(trust_path_));
    EXPECT_TRUE(db.is_trusted(kStudio));
    EXPECT_FALSE(db.is_trusted(kDesk));
}

TEST_F(PairedComputersFiles, aChangeIsReportedOnceTheFileHasSettled)
{
    write_trust({kDesk});
    FileChangeWatch watch(trust_path_);
    EXPECT_FALSE(watch.changed());

    write_trust({kDesk, kStudio});
    EXPECT_FALSE(watch.changed());
    EXPECT_TRUE(watch.changed());
    EXPECT_FALSE(watch.changed());
}

TEST_F(PairedComputersFiles, aFileThatGoesIsNotAChangeUntilItsBack)
{
    write_trust({kDesk});
    FileChangeWatch watch(trust_path_);

    fs::remove(trust_path_);
    EXPECT_FALSE(watch.changed());
    EXPECT_FALSE(watch.changed());
    EXPECT_FALSE(watch.changed());

    write_trust({kStudio});
    EXPECT_FALSE(watch.changed());
    EXPECT_TRUE(watch.changed());
}

} // namespace inputleap
