#include "media/ffmpeg.h"

#include <cerrno>

namespace ge::media::ff {

std::string LibraryVersions() {
  return "avcodec " + std::to_string(LIBAVCODEC_VERSION_MAJOR) + "." + std::to_string(LIBAVCODEC_VERSION_MINOR) +
         " avformat " + std::to_string(LIBAVFORMAT_VERSION_MAJOR) + "." + std::to_string(LIBAVFORMAT_VERSION_MINOR) +
         " avutil " + std::to_string(LIBAVUTIL_VERSION_MAJOR) + "." + std::to_string(LIBAVUTIL_VERSION_MINOR) +
         " (runtime avcodec " + std::to_string(avcodec_version() >> 16) + "." +
         std::to_string((avcodec_version() >> 8) & 0xff) + ")";
}

std::string ErrorString(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(err, buf, sizeof(buf));
  return buf;
}

Status ToStatus(int err, std::string_view what) {
  if (err >= 0) return Status::Ok();
  if (err == AVERROR(EAGAIN)) return Status::WouldBlock();
  if (err == AVERROR(ENOMEM)) {
    return Status::ResourceExhausted(std::string(what) + ": out of memory");
  }
  if (err == AVERROR(ENOENT) || err == AVERROR_STREAM_NOT_FOUND ||
      err == AVERROR_DECODER_NOT_FOUND || err == AVERROR_ENCODER_NOT_FOUND ||
      err == AVERROR_MUXER_NOT_FOUND || err == AVERROR_DEMUXER_NOT_FOUND ||
      err == AVERROR_FILTER_NOT_FOUND) {
    return Status::NotFound(std::string(what) + ": " + ErrorString(err));
  }
  if (err == AVERROR(EINVAL) || err == AVERROR_INVALIDDATA) {
    return Status::InvalidArgument(std::string(what) + ": " + ErrorString(err));
  }
  return Status::Internal(std::string(what) + ": " + ErrorString(err));
}

AVPixelFormat PixelFormatFromName(std::string_view name) noexcept {
  return av_get_pix_fmt(std::string(name).c_str());
}

std::string PixelFormatName(AVPixelFormat fmt) {
  const char* n = av_get_pix_fmt_name(fmt);
  return n == nullptr ? std::string() : std::string(n);
}

AVSampleFormat SampleFormatFromName(std::string_view name) noexcept {
  return av_get_sample_fmt(std::string(name).c_str());
}

std::string SampleFormatName(AVSampleFormat fmt) {
  const char* n = av_get_sample_fmt_name(fmt);
  return n == nullptr ? std::string() : std::string(n);
}

}  // namespace ge::media::ff
