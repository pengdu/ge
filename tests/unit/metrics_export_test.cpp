#include <ge/cpp/metrics_export.h>

#include <gtest/gtest.h>

#include <string>

namespace {

using ge::LatencyHistogram;
using ge::PrometheusWriter;

TEST(PrometheusWriterTest, DeclaresEachMetricOnceAndFormatsSamples) {
  PrometheusWriter w("t_");
  w.Counter("packets_total", "Packets", "session=\"1\"", 5);
  w.Counter("packets_total", "Packets", "session=\"2\"", 7);
  w.Gauge("depth", "Queue depth", "", 2.5);
  const std::string out = w.Finish();
  EXPECT_EQ(out,
            "# HELP t_packets_total Packets\n"
            "# TYPE t_packets_total counter\n"
            "t_packets_total{session=\"1\"} 5\n"
            "t_packets_total{session=\"2\"} 7\n"
            "# HELP t_depth Queue depth\n"
            "# TYPE t_depth gauge\n"
            "t_depth 2.5\n");
}

TEST(PrometheusWriterTest, HistogramIsCumulativeInSecondsWithInf) {
  LatencyHistogram h;
  h.Record(500);       // bucket 0 (< 1024 ns)
  h.Record(1'500);     // bucket 1 (< 2048 ns)
  h.Record(3'000'000); // 3 ms
  PrometheusWriter w("t_");
  w.Histogram("lat_seconds", "Latency", "node=\"a\"", h);
  const std::string out = w.Finish();
  EXPECT_NE(out.find("# TYPE t_lat_seconds histogram\n"), std::string::npos);
  EXPECT_NE(out.find("t_lat_seconds_bucket{node=\"a\",le=\"1.024e-06\"} 1\n"), std::string::npos);
  EXPECT_NE(out.find("t_lat_seconds_bucket{node=\"a\",le=\"2.048e-06\"} 2\n"), std::string::npos);
  EXPECT_NE(out.find("t_lat_seconds_bucket{node=\"a\",le=\"+Inf\"} 3\n"), std::string::npos);
  EXPECT_NE(out.find("t_lat_seconds_sum{node=\"a\"} 0.003002\n"), std::string::npos);
  EXPECT_NE(out.find("t_lat_seconds_count{node=\"a\"} 3\n"), std::string::npos);
  // 16 buckets, one line each, plus sum and count and two header lines.
  std::size_t lines = 0;
  for (const char c : out) {
    if (c == '\n') ++lines;
  }
  EXPECT_EQ(lines, LatencyHistogram::kBuckets + 4);
}

TEST(PrometheusWriterTest, EscapesLabelValues) {
  EXPECT_EQ(PrometheusWriter::EscapeLabel("a\"b\\c\nd"), "a\\\"b\\\\c\\nd");
  PrometheusWriter w;
  w.Gauge("g", "h", "", 1);
  EXPECT_EQ(w.Finish(), "# HELP ge_g h\n# TYPE ge_g gauge\nge_g 1\n");
}

}  // namespace
