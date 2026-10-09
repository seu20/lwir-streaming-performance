#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "common/frame.hpp"
#include "common/stage_metric.hpp"


struct StreamingConfig
{
    std::string host;

    int port = 5004;

    int width = 640;
    int height = 480;
    int fps = 30;
};


class Streaming
{
public:
    explicit Streaming(
        StreamingConfig config,
        bool measurement_enabled = false);
    ~Streaming();

    // GStreamer Pipeline을 생성하고 PLAYING 상태로 전환한다.
    bool open();

    // 전처리가 끝난 CV_8UC1 Frame을 appsrc에 전달한다.
    bool push(const Frame& frame);

    // EOS 전달 후 GStreamer 자원을 해제한다.
    void close();

    // Pipeline 종료 후 수집된 Encoding 측정값의 snapshot을 반환한다.
    std::vector<StageMetric> encoding_metrics() const;

private:
    struct PendingEncoding
    {
        std::uint64_t frame_id = 0;
        std::int64_t captured_system_ns = 0;
        std::chrono::steady_clock::time_point entered_at{};
    };

    struct RtpFrameMetadata
    {
        std::uint64_t frame_id = 0;
        std::int64_t captured_system_ns = 0;
    };

    static GstPadProbeReturn encoder_sink_probe(
        GstPad* pad,
        GstPadProbeInfo* info,
        gpointer user_data);

    static GstPadProbeReturn encoder_src_probe(
        GstPad* pad,
        GstPadProbeInfo* info,
        gpointer user_data);

    static GstPadProbeReturn payloader_src_probe(
        GstPad* pad,
        GstPadProbeInfo* info,
        gpointer user_data);

    void record_encoder_input(GstBuffer* buffer);
    void record_encoder_output(GstBuffer* buffer);
    void add_rtp_metadata(GstPadProbeInfo* info);
    bool install_encoding_probes();
    void remove_encoding_probes();

    StreamingConfig config_;
    bool measurement_enabled_ = false;

    GstElement* pipeline_ = nullptr;

    GstElement* appsrc_ = nullptr;
    GstElement* queue_ = nullptr;
    GstElement* converter_ = nullptr;
    GstElement* encoder_capsfilter_ = nullptr;
    GstElement* encoder_ = nullptr;
    GstElement* parser_ = nullptr;
    GstElement* payloader_ = nullptr;
    GstElement* sink_ = nullptr;

    GstPad* encoder_sink_pad_ = nullptr;
    GstPad* encoder_src_pad_ = nullptr;
    GstPad* payloader_src_pad_ = nullptr;
    gulong encoder_sink_probe_id_ = 0;
    gulong encoder_src_probe_id_ = 0;
    gulong payloader_src_probe_id_ = 0;

    mutable std::mutex encoding_mutex_;
    std::unordered_map<GstClockTime, PendingEncoding>
        pending_encodings_;
    std::unordered_map<GstClockTime, RtpFrameMetadata>
        encoded_frame_metadata_;
    std::vector<StageMetric> encoding_metrics_;
    GstClockTime first_encoder_input_pts_ = GST_CLOCK_TIME_NONE;
    GstClockTime encoder_pts_offset_ = GST_CLOCK_TIME_NONE;

    bool opened_ = false;
};
