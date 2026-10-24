// SPDX-License-Identifier: GPL-3.0-or-later
//
// See tsmux.h.
#include "tsmux.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

namespace {

// MPEG-TS packets are 188 bytes. The muxer writes them one at a time, so
// with a buffer of whole packets every chunk handed on is whole packets
// too, and dropping chunks never splits one.
constexpr int kTsPacket = 188;
constexpr int kAvioBufferSize = kTsPacket * 64;

// How much may wait for mpv. Live TV stays live: when mpv falls this far
// behind, the oldest bytes go. (At broadcast bitrates this is many seconds;
// it only fills when mpv stops reading.)
constexpr size_t kMaxBuffered = 32 * 1024 * 1024;

// Where the rebased timestamps start, so the first packets' DTS (and audio
// that slightly precedes the video) stay positive.
constexpr int64_t kStartUs = 1000000;

// A timestamp this far from its stream's previous one is a discontinuity
// (a source that restarted, a splice), not jitter: the timeline is re-based
// so it carries on from where it was.
constexpr int64_t kJumpUs = 5000000;
constexpr int64_t kJumpStepUs = 40000;

constexpr AVRational kMicroseconds = {1, 1000000};

// HTSP stream type -> codec, for the codecs MPEG-TS can carry.
AVCodecID TsCodec(const std::string& type) {
  if (type == "H264") return AV_CODEC_ID_H264;
  if (type == "HEVC") return AV_CODEC_ID_HEVC;
  if (type == "MPEG2VIDEO") return AV_CODEC_ID_MPEG2VIDEO;
  if (type == "AC3") return AV_CODEC_ID_AC3;
  if (type == "EAC3") return AV_CODEC_ID_EAC3;
  if (type == "AAC") return AV_CODEC_ID_AAC;
  if (type == "MPEG2AUDIO") return AV_CODEC_ID_MP2;
  if (type == "MP3") return AV_CODEC_ID_MP3;
  if (type == "OPUS") return AV_CODEC_ID_OPUS;
  return AV_CODEC_ID_NONE;
}

} // namespace

TsMux::~TsMux() {
  FreeMuxer();
}

void TsMux::FreeMuxer() {
  if (avio_) {
    av_freep(&avio_->buffer);
    avio_context_free(&avio_);
  }
  if (fmt_) {
    fmt_->pb = nullptr;
    avformat_free_context(fmt_);
    fmt_ = nullptr;
  }
  streams_.clear();
  last_dts_.clear();
  base_us_ = INT64_MIN;
}

std::string TsMux::Start(const std::vector<HtspStreamInfo>& streams) {
  FreeMuxer();

  if (avformat_alloc_output_context2(&fmt_, nullptr, "mpegts", nullptr) < 0 || !fmt_)
    return {};

  std::string video_desc, audio_desc;
  for (const HtspStreamInfo& info : streams) {
    const AVCodecID codec = TsCodec(info.type);
    if (codec == AV_CODEC_ID_NONE)
      continue;
    // Opus in MPEG-TS needs its header; without one the muxer refuses it.
    if (codec == AV_CODEC_ID_OPUS && info.extradata.empty())
      continue;

    AVStream* st = avformat_new_stream(fmt_, nullptr);
    if (!st)
      continue;
    const bool video = avcodec_get_type(codec) == AVMEDIA_TYPE_VIDEO;
    AVCodecParameters* par = st->codecpar;
    par->codec_type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    par->codec_id = codec;
    if (video) {
      par->width = (int)info.width;
      par->height = (int)info.height;
    } else {
      par->sample_rate = (int)info.rate;
      if (info.channels > 0)
        av_channel_layout_default(&par->ch_layout, (int)info.channels);
    }
    if (!info.extradata.empty()) {
      par->extradata = static_cast<uint8_t*>(
          av_mallocz(info.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
      if (par->extradata) {
        std::memcpy(par->extradata, info.extradata.data(), info.extradata.size());
        par->extradata_size = (int)info.extradata.size();
      }
    }
    if (!info.language.empty())
      av_dict_set(&st->metadata, "language", info.language.c_str(), 0);
    st->time_base = kMicroseconds;
    streams_[info.index] = st->index;

    // The description names the first video and the first audio stream,
    // which are what mpv picks unless told otherwise.
    std::string& desc = video ? video_desc : audio_desc;
    if (desc.empty()) {
      desc = info.type;
      if (video && info.width && info.height)
        desc += " " + std::to_string(info.width) + "x" + std::to_string(info.height);
    }
  }

  if (streams_.empty()) {
    FreeMuxer();
    return {};
  }

  uint8_t* buffer = static_cast<uint8_t*>(av_malloc(kAvioBufferSize));
  avio_ = buffer ? avio_alloc_context(buffer, kAvioBufferSize, 1, this, nullptr,
#if LIBAVFORMAT_VERSION_MAJOR >= 61
                                      &TsMux::WritePacket,
#else
                                      &TsMux::WritePacketMutable,
#endif
                                      nullptr)
                 : nullptr;
  if (!avio_) {
    av_free(buffer);
    FreeMuxer();
    return {};
  }
  fmt_->pb = avio_;
  // Every packet goes out as soon as it is muxed; mpv is waiting for it.
  fmt_->flags |= AVFMT_FLAG_FLUSH_PACKETS;
  fmt_->max_delay = 0;

  AVDictionary* opts = nullptr;
  // PAT/PMT often enough that the demuxer finds them fast after a restart.
  av_dict_set(&opts, "mpegts_flags", "resend_headers", 0);
  av_dict_set(&opts, "service_name", "tvhp", 0);
  const int rc = avformat_write_header(fmt_, &opts);
  av_dict_free(&opts);
  if (rc < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(rc, err, sizeof(err));
    std::fprintf(stderr, "tsmux: header: %s\n", err);
    FreeMuxer();
    return {};
  }
  last_dts_.assign(fmt_->nb_streams, INT64_MIN);

  std::string desc = video_desc;
  if (!audio_desc.empty())
    desc += (desc.empty() ? "" : " + ") + audio_desc;
  return desc;
}

void TsMux::Write(const HtspMuxPacket& in) {
  if (!fmt_ || in.payload.empty())
    return;
  auto it = streams_.find(in.stream);
  if (it == streams_.end())
    return;
  const int index = it->second;

  int64_t pts = in.pts;
  int64_t dts = in.dts != INT64_MIN ? in.dts : in.pts;
  if (dts == INT64_MIN)
    return; // nothing to place it by; the demuxer couldn't either

  if (base_us_ == INT64_MIN)
    base_us_ = std::min(dts, pts != INT64_MIN ? pts : dts);
  const int64_t last = last_dts_[index];
  if (last != INT64_MIN) {
    const int64_t rel = dts - base_us_ + kStartUs;
    if (rel < last - kJumpUs || rel > last + kJumpUs) {
      std::fprintf(stderr, "tsmux: timestamp discontinuity (%+.1f s), re-basing\n",
                   (rel - last) / 1e6);
      base_us_ += rel - (last + kJumpStepUs);
    }
  }
  dts = dts - base_us_ + kStartUs;
  pts = pts != INT64_MIN ? pts - base_us_ + kStartUs : dts;
  if (dts < 0 || pts < dts)
    return;
  // The muxer insists on increasing DTS per stream.
  if (last != INT64_MIN && dts <= last)
    return;
  last_dts_[index] = dts;

  AVPacket* pkt = av_packet_alloc();
  if (!pkt)
    return;
  if (av_new_packet(pkt, (int)in.payload.size()) < 0) {
    av_packet_free(&pkt);
    return;
  }
  std::memcpy(pkt->data, in.payload.data(), in.payload.size());
  pkt->stream_index = index;
  const AVRational tb = fmt_->streams[index]->time_base;
  pkt->pts = av_rescale_q(pts, kMicroseconds, tb);
  pkt->dts = av_rescale_q(dts, kMicroseconds, tb);
  pkt->duration = in.duration > 0 ? av_rescale_q(in.duration, kMicroseconds, tb) : 0;
  if (in.frametype == 'I' ||
      avcodec_get_type(fmt_->streams[index]->codecpar->codec_id) == AVMEDIA_TYPE_AUDIO)
    pkt->flags |= AV_PKT_FLAG_KEY;

  // Packets arrive interleaved already; av_write_frame() keeps them in that
  // order instead of holding them back to interleave.
  const int rc = av_write_frame(fmt_, pkt);
  if (rc < 0) {
    static bool warned = false;
    if (!warned) {
      char err[AV_ERROR_MAX_STRING_SIZE] = {};
      av_strerror(rc, err, sizeof(err));
      std::fprintf(stderr, "tsmux: dropping packets: %s\n", err);
      warned = true;
    }
  }
  av_packet_free(&pkt);
}

int TsMux::WritePacket(void* opaque, const uint8_t* buf, int size) {
  static_cast<TsMux*>(opaque)->Push(buf, size);
  return size;
}

int TsMux::WritePacketMutable(void* opaque, uint8_t* buf, int size) {
  return WritePacket(opaque, buf, size);
}

void TsMux::Push(const uint8_t* buf, int size) {
  if (size <= 0)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (finished_ || cancelled_)
    return;
  chunks_.emplace_back(buf, buf + size);
  buffered_ += (size_t)size;
  while (buffered_ > kMaxBuffered && chunks_.size() > 1) {
    if (!dropping_) {
      std::fprintf(stderr, "tsmux: the player fell behind, dropping data\n");
      dropping_ = true;
    }
    buffered_ -= chunks_.front().size() - chunk_offset_;
    chunks_.pop_front();
    chunk_offset_ = 0;
  }
  if (buffered_ < kMaxBuffered / 2)
    dropping_ = false;
  cv_.notify_all();
}

void TsMux::Finish() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
  }
  cv_.notify_all();
}

int64_t TsMux::Read(char* buf, uint64_t size) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [&] { return cancelled_ || finished_ || !chunks_.empty(); });
  if (cancelled_)
    return 0;

  uint64_t copied = 0;
  while (copied < size && !chunks_.empty()) {
    std::vector<uint8_t>& chunk = chunks_.front();
    const size_t n = std::min<size_t>(chunk.size() - chunk_offset_, size - copied);
    std::memcpy(buf + copied, chunk.data() + chunk_offset_, n);
    copied += n;
    chunk_offset_ += n;
    buffered_ -= n;
    if (chunk_offset_ == chunk.size()) {
      chunks_.pop_front();
      chunk_offset_ = 0;
    }
  }
  return (int64_t)copied; // 0 only once finished and drained
}

void TsMux::Cancel() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
  }
  cv_.notify_all();
}

void TsMux::Close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
    cancelled_ = true;
    chunks_.clear();
    buffered_ = 0;
  }
  cv_.notify_all();
}
