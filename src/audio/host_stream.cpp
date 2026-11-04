/**
 * @file        audio/host_stream.cpp
 * @brief       One host audio file played over the guest's sound (rex/audio/host_stream.h)
 *
 * - Decode: the whole file is read by the stream's own thread (ID3v2 / ID3v1 tags skipped), cut into MP3 frames by
 *   the mpegaudio parser and decoded by mp3float (FFmpeg 4.4 API: frame->channels), to interleaved stereo float;
 *   other rates are resampled to 48 kHz (linear interpolation).
 * - Hand-over: a ring of 4 s of stereo frames, one writer (the decoder thread) and one reader (the audio callback),
 *   both by atomic counters; the decoder sleeps while the ring is full. A pass ends where the decoder reached the end
 *   of the data (a small queue of those positions), so the fade before the end is known about 4 s ahead.
 * - Life: the decoder thread holds the stream and ends when it is stopped or has nothing left to decode; the stream
 *   is freed by whoever lets go of it last, which is never a join.
 */

#include <rex/audio/host_stream.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/logging.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/samplefmt.h"
}

namespace rex::audio::host_stream {
namespace {

// The file in memory, padded for the parser (it may read AV_INPUT_BUFFER_PADDING_SIZE bytes past the end).
struct FileData {
  std::vector<uint8_t> bytes;
  size_t begin = 0;
  size_t end = 0;
};

bool ReadFile(const std::filesystem::path& file, FileData& out, std::string& error) {
  std::ifstream in(file, std::ios::binary | std::ios::ate);
  if (!in) {
    error = "can't open the file";
    return false;
  }
  const std::streamoff size = in.tellg();
  if (size <= 0 || size > (512ll << 20)) {
    error = size <= 0 ? "empty file" : "file over 512 MB";
    return false;
  }
  out.bytes.assign(size_t(size) + AV_INPUT_BUFFER_PADDING_SIZE, 0);
  in.seekg(0);
  if (!in.read(reinterpret_cast<char*>(out.bytes.data()), size)) {
    error = "read failed";
    return false;
  }
  out.begin = 0;
  out.end = size_t(size);
  const uint8_t* d = out.bytes.data();
  // ID3v2 at the start (syncsafe size, plus a footer when flagged); ID3v1 ("TAG", 128 bytes) at the end.
  if (out.end >= 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
    const size_t tag = (size_t(d[6] & 0x7F) << 21) | (size_t(d[7] & 0x7F) << 14) | (size_t(d[8] & 0x7F) << 7) |
                       size_t(d[9] & 0x7F);
    out.begin = std::min(out.end, 10 + tag + ((d[5] & 0x10) ? 10 : 0));
  }
  if (out.end - out.begin >= 128 && std::memcmp(d + out.end - 128, "TAG", 3) == 0) {
    out.end -= 128;
  }
  return true;
}

class Decoder {
 public:
  ~Decoder() { Close(); }

  bool Open(const FileData* data, std::string& error) {
    data_ = data;
    codec_ = avcodec_find_decoder(AV_CODEC_ID_MP3);  // mp3float first in the codec list
    if (!codec_) {
      error = "no MP3 decoder in this build";
      return false;
    }
    ctx_ = avcodec_alloc_context3(codec_);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!ctx_ || !packet_ || !frame_ || avcodec_open2(ctx_, codec_, nullptr) < 0) {
      error = "MP3 decoder didn't open";
      return false;
    }
    return Rewind(error);
  }

  bool Rewind(std::string& error) {
    if (parser_) {
      av_parser_close(parser_);
    }
    parser_ = av_parser_init(AV_CODEC_ID_MP3);
    if (!parser_) {
      error = "no MPEG audio parser in this build";
      return false;
    }
    avcodec_flush_buffers(ctx_);
    pos_ = data_->begin;
    flushed_ = false;
    have_last_ = false;
    t_ = 0.0;
    return true;
  }

  // Decodes a little more into `out` (interleaved stereo, 48 kHz, appended). False at the end of the data (with the
  // last frames already appended) or on an error (error set).
  bool Step(std::vector<float>& out, std::string& error) {
    if (flushed_) {
      return false;
    }
    uint8_t* packet = nullptr;
    int size = 0;
    if (pos_ < data_->end) {
      const int chunk = int(std::min<size_t>(data_->end - pos_, 16384));
      const int used = av_parser_parse2(parser_, ctx_, &packet, &size, data_->bytes.data() + pos_, chunk,
                                        AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (used < 0) {
        error = "MPEG audio parser error";
        return false;
      }
      pos_ += size_t(used);
      if (size > 0 && !Send(packet, size, out, error)) {
        return false;
      }
      return true;
    }
    av_parser_parse2(parser_, ctx_, &packet, &size, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (size > 0 && !Send(packet, size, out, error)) {
      return false;
    }
    avcodec_send_packet(ctx_, nullptr);
    if (!Receive(out, error)) {
      return false;
    }
    flushed_ = true;
    if (frames_out_ == 0) {
      error = "no audio decoded (not an MP3?)";
    }
    return false;
  }

  uint32_t source_rate = 0;
  uint32_t source_channels = 0;
  uint64_t bad_packets = 0;   // MP3 frames rejected or dropped as damaged
  uint64_t good_packets = 0;

 private:
  bool Send(uint8_t* data, int size, std::vector<float>& out, std::string& error) {
    packet_->data = data;
    packet_->size = size;
    const int ret = avcodec_send_packet(ctx_, packet_);
    if (ret == AVERROR_INVALIDDATA) {
      ++bad_packets;  // a damaged frame: skipped, as a player would
      return true;
    }
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
      error = "MP3 decode error " + std::to_string(ret);
      return false;
    }
    return Receive(out, error);
  }

  bool Receive(std::vector<float>& out, std::string& error) {
    for (;;) {
      const int ret = avcodec_receive_frame(ctx_, frame_);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return true;
      }
      if (ret == AVERROR_INVALIDDATA) {
        ++bad_packets;
        continue;
      }
      if (ret < 0) {
        error = "MP3 decode error " + std::to_string(ret);
        return false;
      }
      Emit(out);
      av_frame_unref(frame_);
    }
  }

  float Sample(int index, int channel) const {
    const int ch = std::min(channel, frame_->channels - 1);
    switch (frame_->format) {
      case AV_SAMPLE_FMT_FLTP:
        return reinterpret_cast<const float*>(frame_->extended_data[ch])[index];
      case AV_SAMPLE_FMT_FLT:
        return reinterpret_cast<const float*>(frame_->extended_data[0])[index * frame_->channels + ch];
      case AV_SAMPLE_FMT_S16P:
        return reinterpret_cast<const int16_t*>(frame_->extended_data[ch])[index] / 32768.0f;
      case AV_SAMPLE_FMT_S16:
        return reinterpret_cast<const int16_t*>(frame_->extended_data[0])[index * frame_->channels + ch] /
               32768.0f;
      default:
        return 0.0f;
    }
  }

  void Emit(std::vector<float>& out) {
    const int n = frame_->nb_samples;
    if (n <= 0 || frame_->channels <= 0 || frame_->sample_rate <= 0) {
      return;
    }
    // A damaged frame the decoder still turned out decodes to values far past full scale (seen: +35 dBFS): it is
    // dropped like one the decoder rejected. A real MP3's overs stay within about +1 dB.
    for (int i = 0; i < n; ++i) {
      if (!(std::fabs(Sample(i, 0)) <= 1.5f) || !(std::fabs(Sample(i, 1)) <= 1.5f)) {  // NaN too
        ++bad_packets;
        return;
      }
    }
    ++good_packets;
    source_rate = uint32_t(frame_->sample_rate);
    source_channels = uint32_t(frame_->channels);
    if (source_rate == kRate) {
      for (int i = 0; i < n; ++i) {
        out.push_back(Sample(i, 0));
        out.push_back(Sample(i, 1));
      }
      frames_out_ += uint64_t(n);
      return;
    }
    // Linear resampling: t_ is the next output sample's place in this block's source frames (-1 = the last frame of
    // the block before).
    const double step = double(source_rate) / double(kRate);
    if (!have_last_) {
      last_l_ = Sample(0, 0);
      last_r_ = Sample(0, 1);
      have_last_ = true;
      t_ = 0.0;
    }
    while (t_ < double(n - 1)) {
      const int i = int(std::floor(t_));
      const float f = float(t_ - i);
      const float l0 = i < 0 ? last_l_ : Sample(i, 0);
      const float r0 = i < 0 ? last_r_ : Sample(i, 1);
      out.push_back(l0 + (Sample(i + 1, 0) - l0) * f);
      out.push_back(r0 + (Sample(i + 1, 1) - r0) * f);
      ++frames_out_;
      t_ += step;
    }
    t_ -= double(n);
    last_l_ = Sample(n - 1, 0);
    last_r_ = Sample(n - 1, 1);
  }

  void Close() {
    if (parser_) {
      av_parser_close(parser_);
      parser_ = nullptr;
    }
    if (ctx_) {
      avcodec_free_context(&ctx_);
    }
    if (packet_) {
      av_packet_free(&packet_);
    }
    if (frame_) {
      av_frame_free(&frame_);
    }
  }

  const FileData* data_ = nullptr;
  const AVCodec* codec_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVCodecParserContext* parser_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  size_t pos_ = 0;
  bool flushed_ = false;
  uint64_t frames_out_ = 0;
  bool have_last_ = false;
  float last_l_ = 0.0f;
  float last_r_ = 0.0f;
  double t_ = 0.0;
};

constexpr uint64_t kRingFrames = uint64_t(kRate) * 4;

struct Stream {
  std::filesystem::path file;
  Options options;
  std::vector<float> ring = std::vector<float>(size_t(kRingFrames) * 2);
  std::atomic<uint64_t> written{0};
  std::atomic<uint64_t> read{0};
  // Pass ends (total frames written when the decoder reached the end of the data), one writer, one reader.
  std::array<std::atomic<uint64_t>, 8> ends{};
  std::atomic<uint32_t> ends_written{0};
  uint32_t ends_read = 0;                  // callback only
  std::atomic<uint64_t> pass_start{0};
  std::atomic<bool> eof{false};            // the decoder wrote its last frame
  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> finished{false};
  std::atomic<uint32_t> passes{0};
  std::atomic<uint64_t> underruns{0};
  std::atomic<float> volume_target{1.0f};
  std::atomic<double> fade_request{-1.0};
  std::mutex error_mutex;
  std::string error;
  // Callback only.
  float volume_now = 1.0f;
  bool fading = false;
  double fade_frames = 0.0;
  double fade_done = 0.0;

  void Fail(const std::string& why) {
    {
      std::lock_guard lock(error_mutex);
      error = why;
    }
    failed = true;
    REXAPU_WARN("host_stream: {}: {}", file.filename().string(), why);
  }

  void Run() {
    FileData data;
    std::string why;
    if (!ReadFile(file, data, why)) {
      Fail(why);
      return;
    }
    Decoder decoder;
    if (!decoder.Open(&data, why)) {
      Fail(why);
      return;
    }
    std::vector<float> pending;
    size_t taken = 0;  // frames of `pending` already in the ring
    uint64_t pass_frames = 0;
    while (!stop && !finished) {
      if (taken * 2 >= pending.size()) {
        pending.clear();
        taken = 0;
        if (!decoder.Step(pending, why)) {
          if (!why.empty()) {
            Fail(why);
            return;
          }
          if (!pending.empty()) {
            continue;  // the flush's last frames first; the next Step returns false with nothing
          }
          // The end of a pass.
          const uint32_t n = ends_written.load(std::memory_order_relaxed);
          if (n - ends_read_snapshot() >= ends.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));  // the callback hasn't caught up
            continue;
          }
          const uint64_t at = written.load(std::memory_order_relaxed);
          ends[n % ends.size()].store(at, std::memory_order_relaxed);
          ends_written.store(n + 1, std::memory_order_release);
          if (passes.load() == 0 && pass_frames == 0) {
            pass_frames = at;
            if (decoder.bad_packets) {
              REXAPU_WARN("host_stream: {}: {} damaged MP3 frames skipped", file.filename().string(),
                          decoder.bad_packets);
            }
          }
          if (!options.loop) {
            eof = true;
            return;
          }
          if (!decoder.Rewind(why)) {
            Fail(why);
            return;
          }
          continue;
        }
        continue;
      }
      const uint64_t w = written.load(std::memory_order_relaxed);
      const uint64_t free_frames = kRingFrames - (w - read.load(std::memory_order_acquire));
      const uint64_t n = std::min<uint64_t>(free_frames, pending.size() / 2 - taken);
      if (n == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      for (uint64_t i = 0; i < n; ++i) {
        const size_t slot = size_t((w + i) % kRingFrames) * 2;
        ring[slot] = pending[(taken + i) * 2];
        ring[slot + 1] = pending[(taken + i) * 2 + 1];
      }
      taken += size_t(n);
      written.store(w + n, std::memory_order_release);
    }
  }

  std::atomic<uint32_t> ends_read_shared{0};
  uint32_t ends_read_snapshot() const { return ends_read_shared.load(std::memory_order_acquire); }
};

std::mutex g_mutex;
std::shared_ptr<Stream> g_stream;

}  // namespace

Measured Measure(const std::filesystem::path& file) {
  Measured m;
  FileData data;
  if (!ReadFile(file, data, m.error)) {
    return m;
  }
  Decoder decoder;
  if (!decoder.Open(&data, m.error)) {
    return m;
  }
  // 100 ms sums of squares (both channels); a 400 ms block is four of them, every 100 ms.
  constexpr uint64_t kSub = kRate / 10;
  std::vector<double> subs;
  double sub = 0.0;
  uint64_t in_sub = 0;
  float peak = 0.0f;
  std::vector<float> out;
  for (;;) {
    out.clear();
    const bool more = decoder.Step(out, m.error);
    for (size_t i = 0; i + 1 < out.size(); i += 2) {
      sub += double(out[i]) * out[i] + double(out[i + 1]) * out[i + 1];
      peak = std::max({peak, std::fabs(out[i]), std::fabs(out[i + 1])});
      if (++in_sub == kSub) {
        subs.push_back(sub);
        sub = 0.0;
        in_sub = 0;
      }
    }
    m.frames += out.size() / 2;
    if (!more) {
      break;
    }
  }
  if (!m.error.empty()) {
    return m;
  }
  if (decoder.bad_packets * 10 > decoder.good_packets + decoder.bad_packets) {
    m.error = "damaged: " + std::to_string(decoder.bad_packets) + " of " +
              std::to_string(decoder.good_packets + decoder.bad_packets) + " MP3 frames bad";
    return m;
  }
  std::vector<double> blocks;  // mean square per block (sum over channels)
  for (size_t i = 0; i + 4 <= subs.size(); ++i) {
    blocks.push_back((subs[i] + subs[i + 1] + subs[i + 2] + subs[i + 3]) / double(4 * kSub));
  }
  auto lk = [](double ms) { return -0.691 + 10.0 * std::log10(std::max(ms, 1e-12)); };
  double sum = 0.0;
  size_t count = 0;
  for (double b : blocks) {
    if (lk(b) > -70.0) {
      sum += b;
      ++count;
    }
  }
  if (!count) {
    m.error = "silent";
    return m;
  }
  const double relative = lk(sum / double(count)) - 10.0;
  sum = 0.0;
  count = 0;
  for (double b : blocks) {
    if (lk(b) > -70.0 && lk(b) > relative) {
      sum += b;
      ++count;
    }
  }
  m.loudness_db = lk(sum / double(std::max<size_t>(count, 1)));
  m.peak_db = 20.0 * std::log10(std::max(double(peak), 1e-10));
  m.source_rate = decoder.source_rate;
  m.source_channels = decoder.source_channels;
  m.ok = true;
  return m;
}

bool Play(const std::filesystem::path& file, const Options& options) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) {
    return false;
  }
  auto stream = std::make_shared<Stream>();
  stream->file = file;
  stream->options = options;
  {
    std::lock_guard lock(g_mutex);
    if (g_stream) {
      g_stream->stop = true;
    }
    g_stream = stream;
  }
  std::thread([stream] { stream->Run(); }).detach();
  return true;
}

void FadeOut(double seconds) {
  std::lock_guard lock(g_mutex);
  if (g_stream) {
    g_stream->fade_request = std::max(0.01, seconds);
  }
}

void Stop() {
  std::lock_guard lock(g_mutex);
  if (g_stream) {
    g_stream->stop = true;
    g_stream.reset();
  }
}

void SetVolume(float linear) {
  std::lock_guard lock(g_mutex);
  if (g_stream) {
    g_stream->volume_target = std::clamp(linear, 0.0f, 4.0f);
  }
}

Status GetStatus() {
  std::shared_ptr<Stream> s;
  {
    std::lock_guard lock(g_mutex);
    s = g_stream;
  }
  Status status;
  if (!s) {
    return status;
  }
  status.failed = s->failed;
  status.finished = s->finished;
  status.active = !s->finished && !s->failed;
  status.passes = s->passes;
  status.underruns = s->underruns;
  status.seconds = double(s->read.load() - std::min(s->read.load(), s->pass_start.load())) / kRate;
  if (status.failed) {
    std::lock_guard lock(s->error_mutex);
    status.error = s->error;
  }
  return status;
}

void MixInto(float* out, uint32_t frames, uint32_t channels, float gain) {
  std::shared_ptr<Stream> s;
  {
    std::lock_guard lock(g_mutex);
    s = g_stream;
  }
  if (!s || s->finished || s->failed || (channels != 2 && channels != 6)) {
    return;
  }
  const Options& o = s->options;
  const double fade_in = std::max(1.0, o.fade_in * kRate);
  const double fade_out = std::max(1.0, o.fade_out * kRate);
  const double request = s->fade_request.exchange(-1.0);
  if (request > 0.0 && !s->fading) {
    s->fading = true;
    s->fade_frames = request * kRate;
    s->fade_done = 0.0;
  }
  // The live volume, eased per block (5.3 ms) towards its target: about 50 ms.
  s->volume_now += (s->volume_target.load() - s->volume_now) * 0.1f;
  const float level = gain * o.gain * s->volume_now;

  uint64_t r = s->read.load(std::memory_order_relaxed);
  const uint64_t w = s->written.load(std::memory_order_acquire);
  uint64_t pass_start = s->pass_start.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < frames; ++i) {
    // The next pass end the decoder has reached, if any.
    uint64_t end = UINT64_MAX;
    if (s->ends_read != s->ends_written.load(std::memory_order_acquire)) {
      end = s->ends[s->ends_read % s->ends.size()].load(std::memory_order_relaxed);
      if (r >= end) {
        ++s->ends_read;
        s->ends_read_shared.store(s->ends_read, std::memory_order_release);
        pass_start = end;
        s->passes.fetch_add(1);
        end = UINT64_MAX;
        if (s->ends_read != s->ends_written.load(std::memory_order_acquire)) {
          end = s->ends[s->ends_read % s->ends.size()].load(std::memory_order_relaxed);
        }
      }
    }
    if (r >= w) {
      if (s->eof) {
        s->finished = true;
      } else {
        s->underruns.fetch_add(1);
      }
      break;
    }
    const double pos = double(r - pass_start);
    const double length = end != UINT64_MAX ? double(end - pass_start) : double(o.frames);
    double env = std::min(1.0, pos / fade_in);
    if (length > 0.0) {
      env = std::min(env, std::max(0.0, (length - pos) / fade_out));
    }
    if (s->fading) {
      env *= std::max(0.0, 1.0 - s->fade_done / s->fade_frames);
      if (++s->fade_done >= s->fade_frames) {
        s->finished = true;
        break;
      }
    }
    const size_t slot = size_t(r % kRingFrames) * 2;
    const float e = float(env) * level;
    out[i * channels] += s->ring[slot] * e;
    out[i * channels + 1] += s->ring[slot + 1] * e;
    ++r;
  }
  s->pass_start.store(pass_start, std::memory_order_relaxed);
  s->read.store(r, std::memory_order_release);
}

}  // namespace rex::audio::host_stream
