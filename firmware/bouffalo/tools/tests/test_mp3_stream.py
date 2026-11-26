# SPDX-License-Identifier: Apache-2.0
"""Exercise the Orion MP3 parser with arbitrary HTTPS chunk boundaries."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class Mp3StreamTests(unittest.TestCase):
    def test_fragmented_tags_frames_and_end_of_stream(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "mp3.cpp"
            binary = Path(directory) / "mp3"
            source.write_text(
                r'''
#include "platform/audio/mp3_stream.hpp"

#include <cassert>
#include <vector>

using namespace micropixel::bouffalo;
constexpr uint8_t kFirstHeader[] = {0xff, 0xfb, 0x90, 0};
constexpr uint8_t kSecondHeader[] = {0xff, 0xfb, 0xa2, 0};

std::vector<uint8_t> Frame(const uint8_t* header, uint8_t marker) {
    std::vector<uint8_t> frame(ParseMp3Frame(header).bytes, marker);
    std::memcpy(frame.data(), header, 4);
    return frame;
}

int main() {
    auto first = Frame(kFirstHeader, 0x31);
    auto second = Frame(kSecondHeader, 0x42);
    const auto format = ParseMp3Frame(first.data());
    assert(first.size() == 417 && second.size() == 523);
    std::memcpy(first.data() + 36, "Xing", 4);
    first[40] = first[41] = first[42] = 0;
    first[43] = 1;
    first[44] = first[45] = 0;
    first[46] = 1;
    first[47] = 7;
    assert(Mp3TaggedFrameCount(first, format) == 263);

    auto vbri = Frame(kFirstHeader, 0);
    std::memcpy(vbri.data() + 36, "VBRI\0\1", 6);
    vbri[53] = 10;
    assert(Mp3TaggedFrameCount(vbri, format) == 10);

    constexpr size_t kTagBytes = 6000;
    std::vector<uint8_t> tag(20 + kTagBytes, 0x55);
    std::memcpy(tag.data(), "ID3", 3);
    tag[3] = 4;
    tag[5] = 0x10;
    tag[6] = tag[7] = 0;
    tag[8] = kTagBytes >> 7;
    tag[9] = kTagBytes & 0x7f;
    tag.insert(tag.end(), {0, 0xfe, 0x01});
    const auto audio_offset = tag.size();
    tag.insert(tag.end(), first.begin(), first.end());
    tag.insert(tag.end(), second.begin(), second.end());
    tag.insert(tag.end(), {'T', 'A', 'G', '0'});

    Mp3Stream stream;
    for (size_t chunk : {1U, 3U, 17U, 1536U, 3072U}) {
        stream.Reset();
        uint32_t frames = 0;
        for (size_t pos = 0; pos < tag.size(); pos += chunk) {
            auto bytes = std::span<const uint8_t>(tag).subspan(pos, std::min(chunk, tag.size() - pos));
            assert(stream.Push(bytes, [&](auto frame, const Mp3Frame& header, uint32_t number) {
                assert(number == frames++ && header.rate == 44100 && header.samples == 1152);
                assert(std::ranges::equal(frame, number ? second : first));
                return true;
            }));
        }
        assert(frames == 2 && stream.Complete());
        assert(stream.PrefixBytes() == audio_offset && stream.Duration() == format.Duration(2));
    }

    const auto sink = [](auto, const Mp3Frame&, uint32_t) { return true; };
    stream.Reset();
    assert(stream.Push(std::span<const uint8_t>(tag).first(tag.size() - 5), sink) && !stream.Complete());
    stream.Reset();
    assert(!stream.Push(std::vector<uint8_t>(4100), sink));
    stream.Reset();
    assert(!stream.Push(tag, [](auto, const Mp3Frame&, uint32_t) { return false; }) && !stream.Complete());
}
''',
                encoding="utf-8",
            )
            subprocess.run(
                [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
                 str(source), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
