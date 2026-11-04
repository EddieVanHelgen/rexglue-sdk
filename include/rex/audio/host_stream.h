/**
 * @file        rex/audio/host_stream.h
 * @brief       One host audio file played over the guest's sound (2026-11-04)
 *
 * A game can add a music file of the player's own (MP3) to the host audio output: it is decoded ahead on a thread of
 * its own with the SDK's FFmpeg (mp3float + the mpegaudio parser), resampled to the output's 48 kHz when needed, and
 * added to each guest frame on the audio callback, after the guest's mix and under the same output gain and
 * audio_mute. Front left / right on a 5.1 device. One stream at a time.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::audio::host_stream {

constexpr uint32_t kRate = 48000;

// A whole decode of a file, to know it before playing it: its length at 48 kHz, its gated loudness (BS.1770 gating
// on 400 ms blocks, 75 % overlap, -70 absolute and -10 relative; no K filter) in dB, its peak. Slow (a few hundred
// ms per minute of music): call it from a worker thread, never the game's.
struct Measured {
  bool ok = false;
  std::string error;
  uint64_t frames = 0;
  double loudness_db = -200.0;
  double peak_db = -200.0;
  uint32_t source_rate = 0;
  uint32_t source_channels = 0;
};
Measured Measure(const std::filesystem::path& file);

struct Options {
  float gain = 1.0f;       // linear, fixed for this stream (a loudness match)
  double fade_in = 2.0;    // seconds, at the start of every pass
  double fade_out = 3.0;   // seconds, before the end of every pass
  bool loop = false;       // after a pass, start again (with the fade in); else finish
  uint64_t frames = 0;     // one pass at 48 kHz (Measure); 0 = learnt when the decoder reaches the end
};

// Starts a file (replacing the stream that was playing, at once). False if the file can't be read; a decode error
// later shows in Status. Any thread.
bool Play(const std::filesystem::path& file, const Options& options);
// Fades the stream out over `seconds`, then it finishes. Any thread.
void FadeOut(double seconds);
// Stops at once and frees the stream (its decoder thread ends on its own). Any thread.
void Stop();
// A live volume on top of the gain (smoothed over about 50 ms). Any thread.
void SetVolume(float linear);

struct Status {
  bool active = false;    // a stream exists and hasn't finished
  bool finished = false;  // it played to its end (or its fade-out) and stopped
  bool failed = false;    // the decoder failed (error says why)
  std::string error;
  double seconds = 0.0;   // played in the current pass
  uint32_t passes = 0;    // passes completed
  uint64_t underruns = 0; // output blocks the decoder wasn't ready for (silence instead)
};
Status GetStatus();

// The audio callback: adds the stream to one block of interleaved float output (`channels` 2 or 6; 6 = front left
// and right), times `gain` (the output gain). Never blocks on the decoder.
void MixInto(float* out, uint32_t frames, uint32_t channels, float gain);

}  // namespace rex::audio::host_stream
