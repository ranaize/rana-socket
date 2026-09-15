#pragma once

#include <cstdint>
#include <string>

namespace rana {
namespace serializer {
namespace audio {

// Decodes an incoming audio buffer into a WAV file's bytes, ready to hand to
// the STT backend. `encoding` tags the input:
//   "pcm16" — raw little-endian signed 16-bit PCM (sample_rate/channels apply)
//   "wav"   — already a WAV container (passed through unchanged)
// Returns false on an unsupported encoding or I/O error.
//
// This library is the ONLY place that knows about audio codecs/format; the
// socket daemon stays codec-free. Resampling and richer decode (opus, mp3, …)
// are extension points owned exclusively here.
bool decode_to_wav(const std::string& raw, const std::string& encoding,
                   uint32_t sample_rate, uint8_t channels, std::string& wav_out);

}  // namespace audio
}  // namespace serializer
}  // namespace rana
