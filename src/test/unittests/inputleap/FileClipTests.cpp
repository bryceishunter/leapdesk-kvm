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

#include "inputleap/FileClip.h"
#include "inputleap/Clipboard.h"
#include "inputleap/ProtocolUtil.h"
#include "inputleap/protocol_types.h"
#include "io/IStream.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

namespace inputleap {

namespace {

FileClip sample_clip()
{
    FileClip clip;
    clip.files.push_back({"report.pdf", 1234567, 133000000000000000ull});
    clip.files.push_back({"na\xC3\xAFve \xE2\x9C\x93.txt", 0, 42});   // "naïve ✓.txt"
    clip.id = FileClip::make_id({"C:\\Users\\me\\report.pdf", "D:\\x\\na\xC3\xAFve \xE2\x9C\x93.txt"},
                                clip.files);
    return clip;
}

// an in-memory stream: what is written can be read back
class BufferStream : public IStream {
public:
    void close() override { }
    std::uint32_t read(void* buffer, std::uint32_t n) override
    {
        n = std::min<std::uint32_t>(n, static_cast<std::uint32_t>(data_.size() - pos_));
        std::memcpy(buffer, data_.data() + pos_, n);
        pos_ += n;
        return n;
    }
    void write(const void* buffer, std::uint32_t n) override
    {
        data_.append(static_cast<const char*>(buffer), n);
    }
    void flush() override { }
    void shutdownInput() override { }
    void shutdownOutput() override { }
    const EventTarget* get_event_target() const override { return nullptr; }
    bool isReady() const override { return pos_ < data_.size(); }
    std::uint32_t getSize() const override
    {
        return static_cast<std::uint32_t>(data_.size() - pos_);
    }

private:
    std::string data_;
    std::size_t pos_ = 0;
};

} // namespace

TEST(FileClipTests, marshall_roundTrip)
{
    FileClip clip = sample_clip();

    FileClip decoded;
    ASSERT_TRUE(FileClip::unmarshall(clip.marshall(), decoded));

    EXPECT_EQ(clip.id, decoded.id);
    ASSERT_EQ(2u, decoded.files.size());
    EXPECT_EQ("report.pdf", decoded.files[0].name);
    EXPECT_EQ(1234567u, decoded.files[0].size);
    EXPECT_EQ(133000000000000000ull, decoded.files[0].modified);
    EXPECT_EQ(clip.files[1].name, decoded.files[1].name);
    EXPECT_EQ(0u, decoded.files[1].size);
}

TEST(FileClipTests, unmarshall_rejectsTruncatedOrPaddedData)
{
    std::string data = sample_clip().marshall();
    FileClip decoded;
    for (std::size_t length = 0; length < data.size(); ++length) {
        EXPECT_FALSE(FileClip::unmarshall(data.substr(0, length), decoded)) << length;
    }
    EXPECT_FALSE(FileClip::unmarshall(data + '\0', decoded));
}

TEST(FileClipTests, unmarshall_rejectsOtherData)
{
    FileClip decoded;
    EXPECT_FALSE(FileClip::unmarshall("", decoded));
    EXPECT_FALSE(FileClip::unmarshall("hello, world", decoded));

    std::string data = sample_clip().marshall();
    data[3] = '2';  // a future version of the encoding
    EXPECT_FALSE(FileClip::unmarshall(data, decoded));
}

TEST(FileClipTests, unmarshall_rejectsEmptyClips)
{
    FileClip clip = sample_clip();
    FileClip decoded;

    FileClip no_files = clip;
    no_files.files.clear();
    EXPECT_FALSE(FileClip::unmarshall(no_files.marshall(), decoded));

    FileClip no_id = clip;
    no_id.id.clear();
    EXPECT_FALSE(FileClip::unmarshall(no_id.marshall(), decoded));

    FileClip long_id = clip;
    long_id.id.assign(65, 'a');
    EXPECT_FALSE(FileClip::unmarshall(long_id.marshall(), decoded));
}

TEST(FileClipTests, unmarshall_rejectsTooManyFiles)
{
    FileClip clip = sample_clip();
    FileClip::File file = clip.files[0];
    clip.files.assign(FileClip::kMaxFiles, file);
    FileClip decoded;
    EXPECT_TRUE(FileClip::unmarshall(clip.marshall(), decoded));

    clip.files.push_back(file);
    EXPECT_FALSE(FileClip::unmarshall(clip.marshall(), decoded));
}

TEST(FileClipTests, unmarshall_rejectsNamesThatAreNotPlainFileNames)
{
    // the pasting screen writes files under these names
    for (const char* name : {"", ".", "..", "../evil.exe", "dir/file", "dir\\file", "C:evil",
                             "tab\there", "bell\x07"}) {
        FileClip clip = sample_clip();
        clip.files[0].name = name;
        FileClip decoded;
        EXPECT_FALSE(FileClip::unmarshall(clip.marshall(), decoded)) << name;
    }
}

TEST(FileClipTests, makeId_isStableAndTellsCopiesApart)
{
    std::vector<FileClip::File> files{{"a.txt", 10, 100}};
    std::string id = FileClip::make_id({"C:\\a.txt"}, files);

    EXPECT_EQ(16u, id.size());
    EXPECT_EQ(id, FileClip::make_id({"C:\\a.txt"}, files));
    EXPECT_NE(id, FileClip::make_id({"C:\\b\\a.txt"}, files));

    std::vector<FileClip::File> changed = files;
    changed[0].size = 11;
    EXPECT_NE(id, FileClip::make_id({"C:\\a.txt"}, changed));
    changed = files;
    changed[0].modified = 101;
    EXPECT_NE(id, FileClip::make_id({"C:\\a.txt"}, changed));

    // how the paths are split into files matters
    std::vector<FileClip::File> two{{"ab", 1, 1}, {"c", 1, 1}};
    EXPECT_NE(FileClip::make_id({"ab", "c"}, two), FileClip::make_id({"a", "bc"}, two));
}

TEST(FileClipTests, transferName_namesTheFileForThePaste)
{
    FileClip clip = sample_clip();
    EXPECT_EQ("leapdesk-0123456789abcdef-1-report.pdf",
              clip.transfer_name("0123456789abcdef", 0));
    EXPECT_EQ("leapdesk-0123456789abcdef-2-" + clip.files[1].name,
              clip.transfer_name("0123456789abcdef", 1));
}

TEST(FileClipTests, isValidPasteRequest)
{
    EXPECT_TRUE(is_valid_paste_request("0123456789abcdef"));
    EXPECT_TRUE(is_valid_paste_request("01234567"));
    EXPECT_FALSE(is_valid_paste_request("0123456"));
    EXPECT_FALSE(is_valid_paste_request(std::string(65, 'a')));
    EXPECT_FALSE(is_valid_paste_request("0123456789ABCDEF"));
    EXPECT_FALSE(is_valid_paste_request("01234567/../x"));
}

TEST(FileClipTests, isValidPasteAddress)
{
    EXPECT_TRUE(is_valid_paste_address("100.81.171.127"));
    EXPECT_TRUE(is_valid_paste_address("fd7a:115c:a1e0::1"));
    EXPECT_TRUE(is_valid_paste_address("surfacestudio"));
    EXPECT_TRUE(is_valid_paste_address("desktop-gp522k1.tail1234.ts.net"));
    EXPECT_FALSE(is_valid_paste_address(""));
    EXPECT_FALSE(is_valid_paste_address("-evil"));
    EXPECT_FALSE(is_valid_paste_address("a b"));
    EXPECT_FALSE(is_valid_paste_address("a\" --flag"));
    EXPECT_FALSE(is_valid_paste_address(std::string(254, 'a')));
}

TEST(FileClipTests, clipboard_carriesFilesNextToOtherFormats)
{
    std::string files = sample_clip().marshall();
    Clipboard source;
    source.open(0);
    source.clear();
    source.add(IClipboard::kText, "hello");
    source.add(IClipboard::kFiles, files);
    source.close();

    Clipboard copy;
    IClipboard::unmarshall(&copy, IClipboard::marshall(&source), 0);

    copy.open(0);
    EXPECT_EQ("hello", copy.get(IClipboard::kText));
    EXPECT_EQ(files, copy.get(IClipboard::kFiles));
    copy.close();
}

TEST(FileClipTests, protocol_messagesWithSeveralStringsRoundTrip)
{
    // writef used to size a %s as if it took two arguments, which only worked
    // while %s came last in a message
    BufferStream stream;
    std::string request = "0123456789abcdef", file_id = "fedcba9876543210";
    std::string address = "100.81.171.127", detail = "sending 2 file(s)";
    ProtocolUtil::writef(&stream, kMsgQFilePaste, &request, &file_id, &address);
    ProtocolUtil::writef(&stream, kMsgDFileStatus, &request, 1u, &detail);

    std::string got_request, got_file_id, got_address, got_detail;
    std::uint8_t state = 0;
    EXPECT_TRUE(ProtocolUtil::readf(&stream, kMsgQFilePaste, &got_request, &got_file_id,
                                    &got_address));
    EXPECT_TRUE(ProtocolUtil::readf(&stream, kMsgDFileStatus, &got_request, &state, &got_detail));
    EXPECT_EQ(request, got_request);
    EXPECT_EQ(file_id, got_file_id);
    EXPECT_EQ(address, got_address);
    EXPECT_EQ(1, state);
    EXPECT_EQ(detail, got_detail);
    EXPECT_EQ(0u, stream.getSize());
}

TEST(FileClipTests, progressDetailRoundTrips)
{
    std::uint64_t sent = 0;
    std::uint64_t total = 0;
    ASSERT_TRUE(parse_file_paste_progress(file_paste_progress(123456789012ull, 987654321098ull),
                                          sent, total));
    EXPECT_EQ(123456789012ull, sent);
    EXPECT_EQ(987654321098ull, total);
    EXPECT_FALSE(parse_file_paste_progress("sending 1 file(s) to x", sent, total));
    EXPECT_FALSE(parse_file_paste_progress("progress 12", sent, total));
    EXPECT_FALSE(parse_file_paste_progress("progress 12 34 and more", sent, total));
}

} // namespace inputleap
