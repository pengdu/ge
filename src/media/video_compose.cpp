#include <ge/media/mix.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "media/operators_internal.h"

namespace ge::media {

namespace {

struct SlotState {
  std::string member;  // business id (diagnostics); the map key is the port
  MixRect rect;
  MixFit fit = MixFit::kCover;
  int z = 0;
  bool visible = true;

  // Frame backlog kept for alignment (MX-S-2/S-4): pending[0] is the oldest.
  std::deque<std::pair<std::int64_t, ff::FramePtr>> pending;
  ff::FramePtr hold;             // last frame actually used for this window
  std::int64_t hold_pts_ns = INT64_MIN;
  std::int64_t hold_use_ns = INT64_MIN;  // when |hold| was last painted
  bool ever_had_frame = false;   // false => placeholder until the first frame
  bool ended = false;            // EOS seen: never expect more frames
  std::uint64_t frozen_windows = 0;
  std::uint64_t placeholder_windows = 0;

  [[nodiscard]] std::int64_t NewestPts() const {
    if (!pending.empty()) return pending.back().first;
    return hold_pts_ns;
  }
  void TrimBacklog(std::int64_t queue_ns, std::int64_t target) {
    while (pending.size() > 1 && pending.front().first < target - queue_ns) pending.pop_front();
  }
  [[nodiscard]] bool HasBacklog() const { return !pending.empty(); }
};

// P7 (docs/04): N-way video composition onto one canvas.
//
// One input port per member slot ("main" + "s1".."s16", docs/04 §4.1), all
// optional except "main", so members join/leave by mutation while the node
// keeps running. Output is produced per reference tick: the reference member
// (main, or the earliest of the rest under reference=auto) defines the
// timeline, every other member contributes the frame closest to the tick
// target within |window_ms|. A member with nothing in the window is
// compensated per |on_missing|: freeze (its last painted frame), black,
// placeholder (background plus a marker box), or skip (the slot keeps the
// background). A frozen member escalates to the placeholder after
// |freeze_upgrade_ms| (docs/04 MX-S-3, Q5).
//
// Layout (rect/fit/z/visible per slot) is read from the hot-updated
// parameters at every tick, so a change is visible in the very next frame
// (MX-L-3); structural changes (canvas size, member ports) need a mutation.
class VideoCompose final : public Operator {
 public:
  explicit VideoCompose(JsonValue options) : options_(std::move(options)) {}

  Status Open(const OpenRequest& r) override {
    ctx_.Capture(r);
    if (const ConnectionContract* c = r.OutputContract("out"); c != nullptr && c->video.has_value()) {
      if (!c->video->pixel_format.empty()) out_pixel_ = c->video->pixel_format;
      if (c->video->width && !c->video->width->Empty()) negotiated_width_ = static_cast<int>(c->video->width->min);
      if (c->video->height && !c->video->height->Empty()) negotiated_height_ = static_cast<int>(c->video->height->min);
    }
    if (out_pixel_.empty()) out_pixel_ = "yuv420p";
    out_fmt_ = ff::PixelFormatFromName(out_pixel_);
    if (out_fmt_ == AV_PIX_FMT_NONE) {
      return Status::InvalidArgument(ctx_.Prefix() + "unsupported output pixel format '" + out_pixel_ + "'");
    }
    if (Status s = Configure(options_, true); !s.ok()) return s;
    if (reference_ == "main" && !slots_.contains(main_port_)) {
      return Status::InvalidArgument(ctx_.Prefix() + "a slot on port 'main' is required when reference=main");
    }
    if (slots_.empty()) return Status::InvalidArgument(ctx_.Prefix() + "at least one member slot is required");
    return Status::Ok();
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      // FLUSH arrives once, after every input port delivered EOS (12 §4.4):
      // drain the backlog, the reference no longer withholds the output.
      for (auto& [port, slot] : slots_) slot.ended = true;
      return Emit(req);
    }
    if (req.parameters != nullptr) {
      if (Status s = Configure(*req.parameters, false); !s.ok()) return s;
    }
    for (std::size_t i = 0; i < req.inputs.size(); ++i) {
      const PacketRef& in = req.inputs[i];
      if (in->is_event() || in->is_eos()) continue;
      const std::string port = i < req.input_ports.size() ? std::string(req.input_ports[i]) : std::string();
      SlotState* slot = FindPort(port);
      if (slot == nullptr) continue;
      const AVFrame* frame = FrameOf(*in);
      if (frame == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "input is not a video frame");
      // Backlog is bounded by the reorder queue: an over-full backlog means
      // the reference is not consuming, so emit what we can instead of
      // growing without limit.
      const std::size_t budget = static_cast<std::size_t>(
          std::max<std::int64_t>(1, queue_ns_ / std::max<std::int64_t>(1, frame_interval_ns_)));
      if (slot->pending.size() < budget) {
        ff::FramePtr copy = ff::NewFrame();
        if (const int err = av_frame_ref(copy.get(), frame); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "frame_ref");
        slot->pending.emplace_back(in->header.pts_ns, std::move(copy));
        slot->ever_had_frame = true;
      }
      ++frames_in_;
    }
    return Emit(req);
  }

  Status Close(const CloseRequest&) override {
    for (auto& [id, slot] : slots_) {
      slot.pending.clear();
      slot.hold.reset();
    }
    return Status::Ok();
  }

 private:
  SlotState* FindPort(const std::string& port) {
    const auto it = slots_.find(port);
    return it == slots_.end() ? nullptr : &it->second;
  }

  // Parses the hot-updated parameters. Structural fields (width/height/fps)
  // may only be set on the first call / in the node options: a live canvas
  // change would invalidate every slot rect, so it is rejected instead of
  // silently applied.
  Status Configure(const JsonValue& params, bool opening) {
    if (opening) {
      width_ = static_cast<int>(options_.GetInteger("width").value_or(negotiated_width_ > 0 ? negotiated_width_ : 1280));
      height_ = static_cast<int>(options_.GetInteger("height").value_or(negotiated_height_ > 0 ? negotiated_height_ : 720));
      fps_ = static_cast<int>(options_.GetInteger("fps").value_or(30));
      gap_ = options_.GetInteger("gap").value_or(0);
      background_ = ParseColor(options_.GetString("background").value_or("black"));
      window_ns_ = options_.GetInteger("window_ms").value_or(40) * 1000000;
      queue_ns_ = options_.GetInteger("queue_ms").value_or(500) * 1000000;
      on_missing_ = ParseMissing(options_.GetString("on_missing").value_or("freeze"));
      freeze_upgrade_ns_ = options_.GetInteger("freeze_upgrade_ms").value_or(10000) * 1000000;
      reference_ = options_.GetString("reference").value_or("main");
      drop_late_ = options_.GetBool("drop_late").value_or(true);
      if (width_ <= 0 || height_ <= 0 || (width_ & 1) != 0 || (height_ & 1) != 0) {
        return Status::InvalidArgument(ctx_.Prefix() + "width/height must be positive and even");
      }
      if (fps_ <= 0) return Status::InvalidArgument(ctx_.Prefix() + "fps must be > 0");
      frame_interval_ns_ = 1000000000 / fps_;
    } else {
      if (params.GetInteger("width").value_or(width_) != width_ ||
          params.GetInteger("height").value_or(height_) != height_ ||
          params.GetInteger("fps").value_or(fps_) != fps_) {
        return Status::InvalidArgument(ctx_.Prefix() + "canvas size/fps are structural; use a mutation to change them");
      }
      if (const auto v = params.GetInteger("gap")) gap_ = *v;
      if (const auto v = params.GetString("background")) background_ = ParseColor(*v);
      if (const auto v = params.GetInteger("window_ms")) window_ns_ = *v * 1000000;
      if (const auto v = params.GetInteger("queue_ms")) queue_ns_ = *v * 1000000;
      if (const auto v = params.GetString("on_missing")) on_missing_ = ParseMissing(*v);
      if (const auto v = params.GetInteger("freeze_upgrade_ms")) freeze_upgrade_ns_ = *v * 1000000;
      if (const auto v = params.GetString("reference")) reference_ = *v;
      if (const auto v = params.GetBool("drop_late")) drop_late_ = *v;
    }
    const JsonValue* slots = params.Find("slots");
    if (slots == nullptr || !slots->is_array()) return Status::Ok();
    // The slots array is authoritative: a member missing from it left the
    // mix, so its slot state (and picture) goes away (docs/04 MX-I-2).
    std::set<std::string> seen;
    for (const JsonValue& s : slots->as_array()) {
      const std::string member = s.GetString("member").value_or("");
      if (member.empty()) return Status::InvalidArgument(ctx_.Prefix() + "slot without a member id");
      const std::string port = s.GetString("port").value_or(member);
      seen.insert(port);
      SlotState& slot = slots_[port];
      slot.member = member;
      const JsonValue* rect = s.Find("rect");
      if (rect == nullptr || !rect->is_object()) return Status::InvalidArgument(ctx_.Prefix() + "slot '" + member + "': rect");
      const MixRect r{static_cast<int>(rect->GetInteger("x").value_or(0)), static_cast<int>(rect->GetInteger("y").value_or(0)),
                      static_cast<int>(rect->GetInteger("w").value_or(0)), static_cast<int>(rect->GetInteger("h").value_or(0))};
      const bool visible = s.GetBool("visible").value_or(true);
      if ((r.w <= 0 || r.h <= 0) && visible) {
        return Status::InvalidArgument(ctx_.Prefix() + "slot '" + member + "': rect needs w/h");
      }
      const std::string fit = s.GetString("fit").value_or("cover");
      if (fit == "cover") slot.fit = MixFit::kCover;
      else if (fit == "contain") slot.fit = MixFit::kContain;
      else return Status::InvalidArgument(ctx_.Prefix() + "slot '" + member + "': fit must be cover|contain");
      slot.rect = r;
      slot.z = static_cast<int>(s.GetInteger("z").value_or(0));
      slot.visible = visible;
    }
    std::erase_if(slots_, [&](const auto& kv) { return !seen.contains(kv.first); });
    return Status::Ok();
  }

  static std::string ParseMissing(const std::string& text) {
    static const char* kAllowed[] = {"freeze", "black", "placeholder", "skip"};
    for (const char* a : kAllowed) {
      if (text == a) return text;
    }
    return "freeze";
  }

  // "black" | "darkgray" | "gray" | "white" | "red" | "green" | "blue" (the
  // small set the canvas and the placeholder marker need).
  static std::uint8_t ParseColor(const std::string& name) {
    if (name == "white") return 235;
    if (name == "darkgray") return 64;
    if (name == "gray") return 128;
    if (name == "red") return 81;
    if (name == "green") return 145;
    if (name == "blue") return 41;
    return 16;  // black
  }

  // Paints |slot| onto |canvas|: swscale into the rect (cover crops the
  // overflow, contain adds a background-coloured matte), then copy the
  // rows. Only Y is written by the fill helper; chroma is left neutral.
  void Paint(SlotState& slot, const AVFrame& frame, AVFrame& canvas) {
    if (slot.rect.w <= 0 || slot.rect.h <= 0) return;
    const int dst_x = std::min(std::max(0, slot.rect.x), width_);
    const int dst_y = std::min(std::max(0, slot.rect.y), height_);
    const int dst_w = std::min(slot.rect.w, width_ - dst_x);
    const int dst_h = std::min(slot.rect.h, height_ - dst_y);
    if (dst_w <= 0 || dst_h <= 0) return;

    const double scale_x = static_cast<double>(dst_w) / frame.width;
    const double scale_y = static_cast<double>(dst_h) / frame.height;
    const double scale = slot.fit == MixFit::kCover ? std::max(scale_x, scale_y) : std::min(scale_x, scale_y);
    const int out_w = std::max(2, static_cast<int>(frame.width * scale) & ~1);
    const int out_h = std::max(2, static_cast<int>(frame.height * scale) & ~1);

    SwsContext* raw = sws_getCachedContext(sws_.release(), frame.width, frame.height, static_cast<AVPixelFormat>(frame.format),
                                           out_w, out_h, out_fmt_, SWS_BILINEAR, nullptr, nullptr, nullptr);
    sws_.reset(raw);
    if (!sws_) return;
    scratch_ = EnsureFrame(std::move(scratch_), out_w, out_h);
    if (!scratch_) return;
    sws_scale(sws_.get(), frame.data, frame.linesize, 0, frame.height, scratch_->data, scratch_->linesize);

    const int off_x = std::max(0, (out_w - dst_w) / 2);
    const int off_y = std::max(0, (out_h - dst_h) / 2);
    for (int y = 0; y < dst_h; ++y) {
      std::memcpy(canvas.data[0] + static_cast<std::ptrdiff_t>(dst_y + y) * canvas.linesize[0] +
                      static_cast<std::ptrdiff_t>(dst_x),
                  scratch_->data[0] + static_cast<std::ptrdiff_t>(off_y + y) * scratch_->linesize[0] + off_x,
                  static_cast<std::size_t>(dst_w));
    }
    const int cx = dst_x / 2;
    const int cy = dst_y / 2;
    const int cw = std::max(1, dst_w / 2);
    const int chh = std::max(1, dst_h / 2);
    for (int y = 0; y < chh; ++y) {
      if (canvas.data[1] != nullptr) {
        std::memset(canvas.data[1] + static_cast<std::ptrdiff_t>(cy + y) * canvas.linesize[1] + cx, 128,
                    static_cast<std::size_t>(cw));
      }
      if (canvas.data[2] != nullptr) {
        std::memset(canvas.data[2] + static_cast<std::ptrdiff_t>(cy + y) * canvas.linesize[2] + cx, 128,
                    static_cast<std::size_t>(cw));
      }
    }
  }

  // The scratch frame is reused across members (its size changes with the
  // slot), so it is re-allocated only when the geometry changes.
  ff::FramePtr EnsureFrame(ff::FramePtr frame, int w, int h) {
    if (frame && frame->width == w && frame->height == h) return frame;
    ff::FramePtr f = ff::NewFrame();
    f->format = out_fmt_;
    f->width = w;
    f->height = h;
    if (av_frame_get_buffer(f.get(), 0) < 0) return nullptr;
    return f;
  }

  void Fill(AVFrame& frame, std::uint8_t luma) {
    for (int y = 0; y < frame.height; ++y) {
      std::memset(frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0], luma,
                  static_cast<std::size_t>(frame.width));
    }
    if (frame.data[1] != nullptr) {
      const int cw = (frame.width + 1) / 2;
      const int ch = (frame.height + 1) / 2;
      for (int y = 0; y < ch; ++y) {
        std::memset(frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1], 128, static_cast<std::size_t>(cw));
        if (frame.data[2] != nullptr) {
          std::memset(frame.data[2] + static_cast<std::ptrdiff_t>(y) * frame.linesize[2], 128, static_cast<std::size_t>(cw));
        }
      }
    }
  }

  // A marker box identifies the placeholder window (the real scene paints
  // the member's name/avatar; libavfilter's drawtext needs freetype, so the
  // box keeps the test surface freetype-free).
  void Marker(const SlotState& slot, AVFrame& canvas) {
    const int bw = std::max(2, slot.rect.w / 8);
    const int bh = std::max(2, slot.rect.h / 8);
    const int x0 = std::max(0, slot.rect.x);
    const int y0 = std::max(0, slot.rect.y);
    for (int y = y0; y < std::min(y0 + bh, height_); ++y) {
      std::memset(canvas.data[0] + static_cast<std::ptrdiff_t>(y) * canvas.linesize[0] + x0, 235,
                  static_cast<std::size_t>(std::min(bw, width_ - x0)));
    }
    for (int y = std::max(0, slot.rect.y + slot.rect.h - bh); y < std::min(slot.rect.y + slot.rect.h, height_); ++y) {
      std::memset(canvas.data[0] + static_cast<std::ptrdiff_t>(y) * canvas.linesize[0] + x0, 235,
                  static_cast<std::size_t>(std::min(bw, width_ - x0)));
    }
  }

  // Chooses the frame to use for |slot| at tick |target|: the newest frame
  // not newer than target + window, or the oldest one when the member is
  // behind (freeze). Late frames older than the window are dropped.
  bool Select(SlotState& slot, std::int64_t target, ff::FramePtr* out, std::int64_t* out_pts) {
    while (!slot.pending.empty() && slot.pending.front().first < target - window_ns_ - queue_ns_) {
      slot.pending.pop_front();
    }
    if (slot.pending.empty()) return false;
    std::size_t best = slot.pending.size();
    for (std::size_t i = 0; i < slot.pending.size(); ++i) {
      const std::int64_t pts = slot.pending[i].first;
      if (pts <= target + window_ns_) best = i;
      else break;
    }
    if (best == slot.pending.size()) return false;  // every frame is ahead of the tick
    if (best > 0) slot.pending.erase(slot.pending.begin(), slot.pending.begin() + static_cast<std::ptrdiff_t>(best));
    *out_pts = slot.pending.front().first;
    *out = std::move(slot.pending.front().second);
    slot.pending.pop_front();
    return true;
  }

  Result<ProcessResult> Emit(const ProcessRequest& req) {
    int emitted = 0;
    while (emitted < kMaxFramesPerCall) {
      SlotState* ref = nullptr;
      std::int64_t target = INT64_MIN;
      if (reference_ == "auto") {
        for (auto& [id, slot] : slots_) {
          if (!slot.visible || !slot.HasBacklog()) continue;
          if (target == INT64_MIN || slot.pending.front().first < target) {
            target = slot.pending.front().first;
            ref = &slot;
          }
        }
      } else if (SlotState* main = FindPort(main_port_); main != nullptr) {
        if (main->HasBacklog()) {
          target = main->pending.front().first;
          ref = main;
        } else if (!main->ended && main->ever_had_frame) {
          // The reference itself is missing this window: hold the output
          // (docs/04 MX-S-6 "freeze output until it recovers").
          return ProcessResult::kContinue;
        } else if (main->ended) {
          // Reference gone (file ended / member left): the earliest of the
          // remaining backlogs keeps the output alive until they drain.
          for (auto& [port, slot] : slots_) {
            if (!slot.visible || !slot.HasBacklog()) continue;
            if (target == INT64_MIN || slot.pending.front().first < target) {
              target = slot.pending.front().first;
              ref = &slot;
            }
          }
        }
      }
      if (ref == nullptr) return ProcessResult::kContinue;
      if (last_out_pts_ns_ != INT64_MIN && target <= last_out_pts_ns_) {
        // Never go backwards: drop the stale window.
        (void)Select(*ref, target, &ref->hold, &ref->hold_pts_ns);
        continue;
      }

      ff::FramePtr canvas = ff::NewFrame();
      canvas->format = out_fmt_;
      canvas->width = width_;
      canvas->height = height_;
      if (const int err = av_frame_get_buffer(canvas.get(), 0); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "canvas");
      Fill(*canvas, background_);

      std::vector<SlotState*> order;
      order.reserve(slots_.size());
      for (auto& [id, slot] : slots_) {
        if (slot.visible) order.push_back(&slot);
      }
      std::sort(order.begin(), order.end(), [](const SlotState* a, const SlotState* b) {
        if (a->z != b->z) return a->z < b->z;
        return a->member < b->member;
      });

      for (SlotState* slot : order) {
        slot->TrimBacklog(queue_ns_, target);
        ff::FramePtr pick;
        std::int64_t pick_pts = INT64_MIN;
        const bool used_frame = Select(*slot, target, &pick, &pick_pts);
        if (used_frame) {
          slot->hold = std::move(pick);
          slot->hold_pts_ns = pick_pts;
          slot->hold_use_ns = target;
          slot->frozen_windows = 0;
          Paint(*slot, *slot->hold, *canvas);
          continue;
        }
        // Nothing in the window: compensate (docs/04 MX-S-3).
        const bool had = slot->hold != nullptr;
        if (had) ++slot->frozen_windows;
        const bool escalate = had && freeze_upgrade_ns_ >= 0 && target - slot->hold_use_ns > freeze_upgrade_ns_;
        std::string mode = on_missing_;
        if (mode == "skip" && had && slot == ref) mode = "freeze";  // the reference cannot leave a hole
        if (escalate) mode = "placeholder";
        if (mode == "skip") continue;
        if (mode == "black") {
          const int x0 = std::max(0, slot->rect.x);
          const int y0 = std::max(0, slot->rect.y);
          for (int y = y0; y < std::min(y0 + slot->rect.h, height_); ++y) {
            std::memset(canvas->data[0] + static_cast<std::ptrdiff_t>(y) * canvas->linesize[0] + x0, 16,
                        static_cast<std::size_t>(std::min(slot->rect.w, width_ - x0)));
          }
          continue;
        }
        if (mode == "placeholder" || !had) {
          ++slot->placeholder_windows;
          Marker(*slot, *canvas);
          continue;
        }
        // freeze: repaint the last frame shipped for this member.
        Paint(*slot, *slot->hold, *canvas);
      }

      const std::int64_t pts = last_out_pts_ns_ == INT64_MIN ? 0 : last_out_pts_ns_ + frame_interval_ns_;
      canvas->pts = pts;
      canvas->time_base = ff::kNanosecond;
      last_out_pts_ns_ = pts;
      // The first composed frame starts a new stream configuration, and a
      // layout change invalidates the previous picture: both want an IDR.
      std::uint32_t flags = 0;
      if (++packets_out_ == 1 || keyframe_pending_) flags |= GE_PACKET_FLAG_KEYFRAME;
      keyframe_pending_ = false;
      MediaFormat fmt = FormatOfFrame(*canvas, MediaKind::kVideo, ff::kNanosecond);
      fmt.frame_rate = Rational{fps_, 1};
      Packet p = MakePacket(WrapFrame(std::move(canvas)), Tag(kTagVideoFrame), ++seq_, pts, pts, flags);
      SetFormat(&p, fmt);
      if (Status s = req.sink->Emit("out", std::move(p)); !s.ok()) return s;
      ++emitted;
    }
    return ProcessResult::kContinue;
  }

  static constexpr int kMaxFramesPerCall = 8;

  JsonValue options_;
  NodeContext ctx_;
  std::string out_pixel_ = "yuv420p";
  AVPixelFormat out_fmt_ = AV_PIX_FMT_YUV420P;
  int negotiated_width_ = 0, negotiated_height_ = 0;
  int width_ = 0, height_ = 0, fps_ = 30;
  std::int64_t gap_ = 0;
  std::uint8_t background_ = 16;
  std::int64_t window_ns_ = 40000000;
  std::int64_t queue_ns_ = 500000000;
  std::int64_t freeze_upgrade_ns_ = 10000000000;
  std::string on_missing_ = "freeze";
  std::string reference_ = "main";
  bool drop_late_ = true;
  std::int64_t frame_interval_ns_ = 33333333;
  std::string main_port_ = "main";
  std::map<std::string, SlotState> slots_;
  ff::SwsContextPtr sws_;
  ff::FramePtr scratch_;
  std::int64_t last_out_pts_ns_ = INT64_MIN;
  std::uint64_t packets_out_ = 0;
  std::uint64_t frames_in_ = 0;
  bool keyframe_pending_ = true;
  PacketSeq seq_ = 0;
};

}  // namespace

std::unique_ptr<Operator> MakeVideoCompose(const OperatorCreateArgs& args) {
  return std::make_unique<VideoCompose>(args.options);
}

}  // namespace ge::media
