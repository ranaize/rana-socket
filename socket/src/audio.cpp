#include "audio.hpp"

#include <cstdint>

namespace rana {
namespace audio {
namespace {

void put_le32(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xff));
    s.push_back(static_cast<char>((v >> 8) & 0xff));
    s.push_back(static_cast<char>((v >> 16) & 0xff));
    s.push_back(static_cast<char>((v >> 24) & 0xff));
}

void put_le16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xff));
    s.push_back(static_cast<char>((v >> 8) & 0xff));
}

}  // namespace

bool decode_to_wav(const std::string& raw, const std::string& encoding,
                   uint32_t sample_rate, uint8_t channels, std::string& wav_out) {
    if (encoding == "wav") {
        wav_out = raw;
        return true;
    }
    if (encoding != "pcm16") return false;

    uint8_t ch = channels == 0 ? 1 : channels;
    uint32_t sr = sample_rate == 0 ? 16000 : sample_rate;

    const uint16_t bits = 16;
    const uint16_t block_align = static_cast<uint16_t>(ch * (bits / 8));
    const uint32_t byte_rate = sr * block_align;

    std::string header;
    header.reserve(44);
    header.append("RIFF");
    put_le32(header, static_cast<uint32_t>(36 + raw.size()));
    header.append("WAVE");
    header.append("fmt ");
    put_le32(header, 16);          // PCM fmt chunk size
    put_le16(header, 1);           // audio format = PCM
    put_le16(header, ch);
    put_le32(header, sr);
    put_le32(header, byte_rate);
    put_le16(header, block_align);
    put_le16(header, bits);
    header.append("data");
    put_le32(header, static_cast<uint32_t>(raw.size()));

    wav_out = header + raw;
    return true;
}

}  // namespace audio
}  // namespace rana
