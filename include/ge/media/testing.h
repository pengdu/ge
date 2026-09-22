#ifndef GE_MEDIA_TESTING_H_
#define GE_MEDIA_TESTING_H_

// file (testsrc2 + sine through libavfilter, encoded with the same encoder
// VideoEncode would pick) and probe an output file through libavformat.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge::media {

struct SampleSpec {
  std::string path;
  int width = 640;
  int height = 360;
  int fps = 30;
  int frames = 90;
  int gop = 30;
  bool audio = true;
  int sample_rate = 48000;
  std::string container;  // derived from extension when empty
};

// Writes |spec.path|; returns the codec names used.
struct SampleInfo {
  std::string video_codec;
  std::string audio_codec;
  std::int64_t duration_ms = 0;
};
[[nodiscard]] Result<SampleInfo> GenerateSample(const SampleSpec& spec);

struct KeyFrameInfo {
  std::int64_t index = 0;   // packet index within the stream
  std::int64_t pts_ms = 0;
};

struct ProbeResult {
  std::string container;
  std::string video_codec;
  std::string audio_codec;
  int width = 0, height = 0;
  std::int64_t video_packets = 0;
  std::int64_t audio_packets = 0;
  std::int64_t video_keyframes = 0;
  bool first_video_is_key = false;
  std::int64_t first_video_pts_ms = INT64_MIN;
  std::int64_t first_audio_pts_ms = INT64_MIN;
  std::int64_t last_video_pts_ms = INT64_MIN;
  std::int64_t last_audio_pts_ms = INT64_MIN;
  std::int64_t av_offset_ms = 0;  // |first_video - first_audio|
  std::int64_t duration_ms = 0;
  std::int64_t total_bytes = 0;
  bool read_to_eof = false;   // av_read_frame reached EOF without error
  bool trailer_ok = false;    // container-specific end marker present
  std::vector<KeyFrameInfo> keyframes;
  std::vector<std::int64_t> video_pts_ms;  // in file order
  [[nodiscard]] JsonValue ToJson() const;
};
[[nodiscard]] Result<ProbeResult> ProbeOutput(const std::string& path);

// Decodes the video stream and returns the average luma of a box at the
// given frame index (watermark check). -1 on failure.
[[nodiscard]] double AverageLuma(const std::string& path, std::int64_t frame_index, int x, int y, int w, int h);

// Runs ffprobe if present; returns the video frame count it reports.
[[nodiscard]] std::optional<std::int64_t> FfprobeVideoFrames(const std::string& path);

}  // namespace ge::media

#endif
