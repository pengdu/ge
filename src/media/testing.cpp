#include <ge/media/testing.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "media/operators_internal.h"

namespace ge::media {

namespace {

struct LavfiSource {
  ff::FilterGraphPtr graph;
  AVFilterContext* sink = nullptr;

  Status Open(const std::string& description, std::string_view what) {
    graph.reset(avfilter_graph_alloc());
    AVFilterInOut* inputs = nullptr;
    AVFilterInOut* outputs = nullptr;
    int err = avfilter_graph_parse2(graph.get(), description.c_str(), &inputs, &outputs);
    if (err < 0) return ff::ToStatus(err, std::string(what) + " parse");
    if (outputs == nullptr) return Status::Internal(std::string(what) + ": no open output");
    const char* sink_name = outputs->filter_ctx->output_pads != nullptr &&
                                    avfilter_pad_get_type(outputs->filter_ctx->output_pads, outputs->pad_idx) == AVMEDIA_TYPE_AUDIO
                                ? "abuffersink"
                                : "buffersink";
    err = avfilter_graph_create_filter(&sink, avfilter_get_by_name(sink_name), "sink", nullptr, nullptr, graph.get());
    if (err >= 0) err = avfilter_link(outputs->filter_ctx, static_cast<unsigned>(outputs->pad_idx), sink, 0);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (err < 0) return ff::ToStatus(err, std::string(what) + " link");
    err = avfilter_graph_config(graph.get(), nullptr);
    if (err < 0) return ff::ToStatus(err, std::string(what) + " config");
    return Status::Ok();
  }

  // Returns nullptr at EOF.
  ff::FramePtr Next() {
    ff::FramePtr f = ff::NewFrame();
    const int err = av_buffersink_get_frame(sink, f.get());
    if (err < 0) return nullptr;
    return f;
  }
};

struct EncStream {
  const AVCodec* codec = nullptr;
  ff::CodecContextPtr ctx;
  AVStream* stream = nullptr;

  Status Drain(AVFormatContext* fmt) {
    for (;;) {
      ff::PacketPtr pkt = ff::NewPacket();
      const int err = avcodec_receive_packet(ctx.get(), pkt.get());
      if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return Status::Ok();
      if (err < 0) return ff::ToStatus(err, "receive");
      // libx264 leaves duration 0; the MP4 edit list then ends before the
      // last sample and demuxers discard that frame.
      if (pkt->duration == 0 && ctx->codec_type == AVMEDIA_TYPE_VIDEO) pkt->duration = 1;
      av_packet_rescale_ts(pkt.get(), ctx->time_base, stream->time_base);
      pkt->stream_index = stream->index;
      if (const int werr = av_interleaved_write_frame(fmt, pkt.get()); werr < 0) return ff::ToStatus(werr, "write");
    }
  }
  Status Send(const AVFrame* frame, AVFormatContext* fmt) {
    if (const int err = avcodec_send_frame(ctx.get(), frame); err < 0) return ff::ToStatus(err, "send");
    return Drain(fmt);
  }
};

}  // namespace

Result<SampleInfo> GenerateSample(const SampleSpec& spec) {
  SampleInfo info;
  std::string container = spec.container;
  if (container.empty()) {
    const std::string ext = std::filesystem::path(spec.path).extension().string();
    container = ext == ".flv" ? "flv" : ext == ".mkv" ? "matroska" : "mp4";
  }
  AVFormatContext* raw = nullptr;
  if (const int err = avformat_alloc_output_context2(&raw, nullptr, container.c_str(), spec.path.c_str()); err < 0) {
    return ff::ToStatus(err, "output context");
  }
  ff::OutputFormatPtr fmt(raw);

  // Video.
  std::ostringstream vdesc;
  vdesc << "testsrc2=size=" << spec.width << "x" << spec.height << ":rate=" << spec.fps << ":duration="
        << (static_cast<double>(spec.frames) / spec.fps) << ",format=yuv420p";
  LavfiSource vsrc;
  if (Status s = vsrc.Open(vdesc.str(), "testsrc2"); !s.ok()) return s;
  EncStream venc;
  venc.codec = FindVideoEncoder("h264", CodecBackend::kSoftware);
  if (venc.codec == nullptr) return Status::NotFound("no video encoder");
  venc.ctx.reset(avcodec_alloc_context3(venc.codec));
  venc.ctx->width = spec.width;
  venc.ctx->height = spec.height;
  venc.ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  venc.ctx->time_base = AVRational{1, spec.fps};
  venc.ctx->framerate = AVRational{spec.fps, 1};
  venc.ctx->gop_size = spec.gop;
  venc.ctx->max_b_frames = 0;
  venc.ctx->bit_rate = 1500000;
  if (fmt->oformat->flags & AVFMT_GLOBALHEADER) venc.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (std::string(venc.codec->name) == "libx264") {
    (void)av_opt_set(venc.ctx->priv_data, "preset", "veryfast", 0);
    (void)av_opt_set(venc.ctx->priv_data, "x264-params", "scenecut=0", 0);
  }
  if (const int err = avcodec_open2(venc.ctx.get(), venc.codec, nullptr); err < 0) return ff::ToStatus(err, "open video encoder");
  venc.stream = avformat_new_stream(fmt.get(), nullptr);
  if (const int err = avcodec_parameters_from_context(venc.stream->codecpar, venc.ctx.get()); err < 0) return ff::ToStatus(err, "vpar");
  venc.stream->time_base = venc.ctx->time_base;
  info.video_codec = venc.codec->name;

  // Audio.
  LavfiSource asrc;
  EncStream aenc;
  if (spec.audio) {
    std::ostringstream adesc;
    adesc << "sine=frequency=440:sample_rate=" << spec.sample_rate << ":duration=" << (static_cast<double>(spec.frames) / spec.fps)
          << ",aformat=sample_fmts=fltp:channel_layouts=stereo";
    if (Status s = asrc.Open(adesc.str(), "sine"); !s.ok()) return s;
    aenc.codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (aenc.codec == nullptr) return Status::NotFound("no aac encoder");
    aenc.ctx.reset(avcodec_alloc_context3(aenc.codec));
    aenc.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    aenc.ctx->sample_rate = spec.sample_rate;
    av_channel_layout_default(&aenc.ctx->ch_layout, 2);
    aenc.ctx->bit_rate = 128000;
    aenc.ctx->time_base = AVRational{1, spec.sample_rate};
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) aenc.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (const int err = avcodec_open2(aenc.ctx.get(), aenc.codec, nullptr); err < 0) return ff::ToStatus(err, "open aac");
    aenc.stream = avformat_new_stream(fmt.get(), nullptr);
    if (const int err = avcodec_parameters_from_context(aenc.stream->codecpar, aenc.ctx.get()); err < 0) return ff::ToStatus(err, "apar");
    aenc.stream->time_base = aenc.ctx->time_base;
    info.audio_codec = "aac";
  }

  if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
    if (const int err = avio_open(&fmt->pb, spec.path.c_str(), AVIO_FLAG_WRITE); err < 0) return ff::ToStatus(err, "open output");
  }
  if (const int err = avformat_write_header(fmt.get(), nullptr); err < 0) return ff::ToStatus(err, "write header");

  // Interleave by feeding both sources alternately; the muxer interleaves.
  std::int64_t vframes = 0;
  for (;;) {
    ff::FramePtr vf = vsrc.Next();
    if (!vf) break;
    vf->pts = vframes++;
    vf->duration = 1;  // keeps the MP4 edit list covering the last sample
    vf->pict_type = AV_PICTURE_TYPE_NONE;
    if (Status s = venc.Send(vf.get(), fmt.get()); !s.ok()) return s;
    if (spec.audio) {
      // Roughly one audio frame per video frame keeps interleaving tight.
      for (int k = 0; k < 2; ++k) {
        ff::FramePtr af = asrc.Next();
        if (!af) break;
        if (Status s = aenc.Send(af.get(), fmt.get()); !s.ok()) return s;
      }
    }
  }
  if (Status s = venc.Send(nullptr, fmt.get()); !s.ok()) return s;
  if (spec.audio) {
    for (;;) {
      ff::FramePtr af = asrc.Next();
      if (!af) break;
      if (Status s = aenc.Send(af.get(), fmt.get()); !s.ok()) return s;
    }
    if (Status s = aenc.Send(nullptr, fmt.get()); !s.ok()) return s;
  }
  if (const int err = av_write_trailer(fmt.get()); err < 0) return ff::ToStatus(err, "write trailer");
  info.duration_ms = vframes * 1000 / spec.fps;
  return info;
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

namespace {

bool TrailerOk(const std::string& path, const std::string& container) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (container == "flv") {
    // Last 4 bytes: PreviousTagSize of the final tag == 11 + last tag's DataSize.
    if (size < 15) return false;
    in.seekg(size - 4);
    std::array<unsigned char, 4> tail{};
    in.read(reinterpret_cast<char*>(tail.data()), 4);
    const std::uint32_t prev = (static_cast<std::uint32_t>(tail[0]) << 24) | (static_cast<std::uint32_t>(tail[1]) << 16) |
                               (static_cast<std::uint32_t>(tail[2]) << 8) | tail[3];
    if (prev < 11 || static_cast<std::streamoff>(prev) + 4 > size) return false;
    in.seekg(size - 4 - static_cast<std::streamoff>(prev));
    std::array<unsigned char, 11> hdr{};
    in.read(reinterpret_cast<char*>(hdr.data()), 11);
    const std::uint32_t data_size = (static_cast<std::uint32_t>(hdr[1]) << 16) | (static_cast<std::uint32_t>(hdr[2]) << 8) | hdr[3];
    return data_size + 11 == prev;
  }
  if (container == "mov,mp4,m4a,3gp,3g2,mj2" || container == "mp4") {
    // Walk top-level boxes; moov must exist and boxes must tile the file.
    std::streamoff pos = 0;
    bool moov = false;
    while (pos + 8 <= size) {
      in.seekg(pos);
      std::array<unsigned char, 8> h{};
      in.read(reinterpret_cast<char*>(h.data()), 8);
      std::uint64_t box = (static_cast<std::uint64_t>(h[0]) << 24) | (static_cast<std::uint64_t>(h[1]) << 16) |
                          (static_cast<std::uint64_t>(h[2]) << 8) | h[3];
      const std::string type(reinterpret_cast<const char*>(h.data()) + 4, 4);
      if (box == 1) {
        std::array<unsigned char, 8> l{};
        in.read(reinterpret_cast<char*>(l.data()), 8);
        box = 0;
        for (unsigned char c : l) box = (box << 8) | c;
      } else if (box == 0) {
        box = static_cast<std::uint64_t>(size - pos);
      }
      if (box < 8) return false;
      if (type == "moov") moov = true;
      pos += static_cast<std::streamoff>(box);
    }
    return moov && pos == size;
  }
  return true;
}

}  // namespace

Result<ProbeResult> ProbeOutput(const std::string& path) {
  ProbeResult r;
  AVFormatContext* raw = nullptr;
  if (const int err = avformat_open_input(&raw, path.c_str(), nullptr, nullptr); err < 0) return ff::ToStatus(err, "open '" + path + "'");
  ff::InputFormatPtr fmt(raw);
  if (const int err = avformat_find_stream_info(fmt.get(), nullptr); err < 0) return ff::ToStatus(err, "stream info");
  r.container = fmt->iformat->name;
  const int vi = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  const int ai = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (vi >= 0) {
    r.video_codec = avcodec_get_name(fmt->streams[vi]->codecpar->codec_id);
    r.width = fmt->streams[vi]->codecpar->width;
    r.height = fmt->streams[vi]->codecpar->height;
  }
  if (ai >= 0) r.audio_codec = avcodec_get_name(fmt->streams[ai]->codecpar->codec_id);
  std::int64_t vindex = 0;
  for (;;) {
    ff::PacketPtr pkt = ff::NewPacket();
    const int err = av_read_frame(fmt.get(), pkt.get());
    if (err == AVERROR_EOF) {
      r.read_to_eof = true;
      break;
    }
    if (err < 0) break;
    const AVStream* st = fmt->streams[pkt->stream_index];
    const std::int64_t pts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    const std::int64_t pts_ms = pts == AV_NOPTS_VALUE ? INT64_MIN : av_rescale_q(pts, st->time_base, AVRational{1, 1000});
    r.total_bytes += pkt->size;
    if (pkt->stream_index == vi) {
      const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
      if (r.video_packets == 0) {
        r.first_video_is_key = key;
        r.first_video_pts_ms = pts_ms;
      }
      if (key) {
        ++r.video_keyframes;
        r.keyframes.push_back({vindex, pts_ms});
      }
      r.video_pts_ms.push_back(pts_ms);
      r.last_video_pts_ms = pts_ms;
      ++r.video_packets;
      ++vindex;
    } else if (pkt->stream_index == ai) {
      if (r.audio_packets == 0) r.first_audio_pts_ms = pts_ms;
      r.last_audio_pts_ms = pts_ms;
      ++r.audio_packets;
    }
  }
  if (r.first_video_pts_ms != INT64_MIN && r.first_audio_pts_ms != INT64_MIN) {
    r.av_offset_ms = std::abs(r.first_video_pts_ms - r.first_audio_pts_ms);
  }
  r.duration_ms = fmt->duration == AV_NOPTS_VALUE ? 0 : fmt->duration / 1000;
  r.trailer_ok = TrailerOk(path, r.container);
  return r;
}

JsonValue ProbeResult::ToJson() const {
  JsonObject o;
  o.emplace("container", JsonValue(container));
  o.emplace("video_codec", JsonValue(video_codec));
  o.emplace("audio_codec", JsonValue(audio_codec));
  o.emplace("width", JsonValue(width));
  o.emplace("height", JsonValue(height));
  o.emplace("video_packets", JsonValue(video_packets));
  o.emplace("audio_packets", JsonValue(audio_packets));
  o.emplace("video_keyframes", JsonValue(video_keyframes));
  o.emplace("first_video_is_key", JsonValue(first_video_is_key));
  o.emplace("first_video_pts_ms", JsonValue(first_video_pts_ms));
  o.emplace("first_audio_pts_ms", JsonValue(first_audio_pts_ms));
  o.emplace("last_video_pts_ms", JsonValue(last_video_pts_ms));
  o.emplace("last_audio_pts_ms", JsonValue(last_audio_pts_ms));
  o.emplace("av_offset_ms", JsonValue(av_offset_ms));
  o.emplace("duration_ms", JsonValue(duration_ms));
  o.emplace("total_bytes", JsonValue(total_bytes));
  o.emplace("read_to_eof", JsonValue(read_to_eof));
  o.emplace("trailer_ok", JsonValue(trailer_ok));
  JsonArray keys;
  for (const KeyFrameInfo& k : keyframes) {
    keys.push_back(JsonValue(JsonObject{{"index", JsonValue(k.index)}, {"pts_ms", JsonValue(k.pts_ms)}}));
  }
  o.emplace("keyframes", JsonValue(std::move(keys)));
  return JsonValue(std::move(o));
}

double AverageLuma(const std::string& path, std::int64_t frame_index, int x, int y, int w, int h) {
  AVFormatContext* raw = nullptr;
  if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0) return -1;
  ff::InputFormatPtr fmt(raw);
  if (avformat_find_stream_info(fmt.get(), nullptr) < 0) return -1;
  const int vi = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vi < 0) return -1;
  const AVCodec* dec = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
  if (dec == nullptr) return -1;
  ff::CodecContextPtr ctx(avcodec_alloc_context3(dec));
  if (avcodec_parameters_to_context(ctx.get(), fmt->streams[vi]->codecpar) < 0) return -1;
  if (avcodec_open2(ctx.get(), dec, nullptr) < 0) return -1;
  std::int64_t decoded = 0;
  bool eof = false;
  while (!eof) {
    ff::PacketPtr pkt = ff::NewPacket();
    const int err = av_read_frame(fmt.get(), pkt.get());
    if (err < 0) {
      eof = true;
      (void)avcodec_send_packet(ctx.get(), nullptr);
    } else {
      if (pkt->stream_index != vi) continue;
      if (avcodec_send_packet(ctx.get(), pkt.get()) < 0) return -1;
    }
    for (;;) {
      ff::FramePtr f = ff::NewFrame();
      const int rerr = avcodec_receive_frame(ctx.get(), f.get());
      if (rerr < 0) break;
      if (decoded++ != frame_index) continue;
      if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P && f->format != AV_PIX_FMT_NV12) {
        return -1;
      }
      double sum = 0;
      std::int64_t n = 0;
      for (int yy = std::max(0, y); yy < std::min(f->height, y + h); ++yy) {
        for (int xx = std::max(0, x); xx < std::min(f->width, x + w); ++xx) {
          sum += f->data[0][yy * f->linesize[0] + xx];
          ++n;
        }
      }
      return n == 0 ? -1 : sum / static_cast<double>(n);
    }
  }
  return -1;
}

std::optional<std::int64_t> FfprobeVideoFrames(const std::string& path) {
  for (const char* bin : {"/opt/homebrew/bin/ffprobe", "/usr/local/bin/ffprobe", "/usr/bin/ffprobe"}) {
    if (!std::filesystem::exists(bin)) continue;
    const std::string cmd = std::string(bin) +
                            " -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 \"" +
                            path + "\" 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) return std::nullopt;
    char buf[64] = {};
    std::string out;
    while (fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
    pclose(pipe);
    try {
      return std::stoll(out);
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

}  // namespace ge::media
