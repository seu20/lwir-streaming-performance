#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

struct ReceiverConfig
{
    int port = 5004;
    bool measurement_enabled = true;
    bool clocks_synchronized = false;
    bool recording_enabled = false;
    std::string recording_output_path;
    double recording_fps = 30.0;
    std::string metrics_output_dir;
    double idle_timeout_seconds = 0.0;
    int udp_buffer_bytes = -1;
    int jitter_latency_ms = -1;
};

class Receiver
{
public:
    explicit Receiver(ReceiverConfig config = {});
    ~Receiver();

    bool open();
    void run();
    void close();

private:
    struct ReceivedMetadata
    {
        std::uint64_t frame_id = 0;
        std::int64_t captured_system_ns = 0;
    };

    struct E2eMetric
    {
        std::uint64_t frame_id = 0;
        double e2e_ms = 0.0;
    };

    struct FpsMetric
    {
        double interval_start_s = 0.0;
        double interval_end_s = 0.0;
        std::uint64_t frame_count = 0;
        double fps = 0.0;
    };

    struct RecordingTimestamp
    {
        std::uint64_t frame_index = 0;
        double received_at_ms = 0.0;
    };

    struct PtsTrace
    {
        std::string stage;
        std::uint64_t sequence = 0;
        GstClockTime pts = GST_CLOCK_TIME_NONE;
        GstClockTime dts = GST_CLOCK_TIME_NONE;
        GstClockTime duration = GST_CLOCK_TIME_NONE;
    };

    static GstPadProbeReturn rtp_probe(
        GstPad* pad,
        GstPadProbeInfo* info,
        gpointer user_data);

    static GstPadProbeReturn appsink_probe(
        GstPad* pad,
        GstPadProbeInfo* info,
        gpointer user_data);

    static GstPadProbeReturn pts_trace_probe(
        GstPad* pad,
        GstPadProbeInfo* info,
        gpointer user_data);

    void receive_rtp_metadata(GstPadProbeInfo* info);
    GstBuffer* identify_depayloaded_access_unit(
        GstBuffer* buffer);
    void associate_access_unit(GstBuffer* buffer);
    void record_pts_trace(GstPad* pad, GstBuffer* buffer);
    std::unordered_map<GstClockTime, ReceivedMetadata>::iterator
        find_metadata_for_decoder_pts(GstClockTime decoder_pts);
    void record_e2e(
        GstBuffer* buffer,
        std::chrono::system_clock::time_point received_at);
    void record_received_frame(
        GstBuffer* buffer,
        std::chrono::steady_clock::time_point received_at);
    void finish_fps_measurement(
        std::chrono::steady_clock::time_point finished_at);
    void save_e2e_metrics_csv() const;
    void save_fps_metrics_csv() const;
    void save_frame_metrics_csv() const;
    void save_pts_trace_csv() const;
    void print_measurement_summary() const;
    bool prepare_recording_output();
    bool initialize_video_writer(const cv::Size& frame_size);
    bool record_frame(
        const cv::Mat& frame,
        std::chrono::steady_clock::time_point received_at);
    void finish_recording();
    void save_recording_timestamps_csv() const;
    std::string metric_path(const char* filename) const;
    bool install_e2e_probes();
    void remove_e2e_probes();

    int port_;
    bool measurement_enabled_ = true;
    bool clocks_synchronized_ = false;
    bool recording_enabled_ = false;
    std::string output_path_;
    std::string timestamp_output_path_;
    double recording_fps_ = 30.0;
    std::string metrics_output_dir_;
    double idle_timeout_seconds_ = 0.0;
    int udp_buffer_bytes_ = -1;
    int jitter_latency_ms_ = -1;

    cv::VideoWriter video_writer_;
    cv::Size recording_frame_size_;
    std::chrono::steady_clock::time_point
        recording_started_at_{};
    std::vector<RecordingTimestamp>
        recording_timestamps_;
    bool recording_initialized_ = false;
    bool recording_prepared_ = false;
    bool recording_failed_ = false;
    bool recording_finished_ = false;

    GstElement* pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;
    GstElement* rtp_source_ = nullptr;
    GstElement* depayloader_ = nullptr;
    GstElement* parser_ = nullptr;
    GstElement* decoder_ = nullptr;

    GstPad* rtp_source_pad_ = nullptr;
    GstPad* appsink_sink_pad_ = nullptr;
    GstPad* depayloader_src_pad_ = nullptr;
    GstPad* parser_src_pad_ = nullptr;
    GstPad* decoder_sink_pad_ = nullptr;
    GstPad* decoder_src_pad_ = nullptr;
    gulong rtp_probe_id_ = 0;
    gulong appsink_probe_id_ = 0;
    gulong depayloader_src_probe_id_ = 0;
    gulong parser_src_probe_id_ = 0;
    gulong decoder_sink_probe_id_ = 0;
    gulong decoder_src_probe_id_ = 0;

    mutable std::mutex metrics_mutex_;
    std::unordered_map<guint32, ReceivedMetadata>
        rtp_metadata_;
    std::deque<guint32> rtp_metadata_order_;
    std::deque<ReceivedMetadata>
        depayloaded_access_units_;
    std::unordered_map<GstClockTime, ReceivedMetadata>
        pending_metadata_;
    std::deque<GstClockTime> pending_order_;
    std::vector<E2eMetric> e2e_metrics_;
    std::vector<FpsMetric> fps_metrics_;
    std::vector<PtsTrace> pts_traces_;
    std::uint64_t depayloader_src_sequence_ = 0;
    std::uint64_t parser_src_sequence_ = 0;
    std::uint64_t decoder_sink_sequence_ = 0;
    std::uint64_t decoder_src_sequence_ = 0;
    std::chrono::steady_clock::time_point fps_started_at_{};
    std::chrono::steady_clock::time_point
        fps_interval_started_at_{};
    std::uint64_t fps_interval_frame_count_ = 0;
    double fps_duration_seconds_ = 0.0;

    std::uint64_t decoded_frame_count_ = 0;
    std::uint64_t metadata_matched_frame_count_ = 0;
    std::uint64_t missing_frame_id_count_ = 0;
    std::uint64_t observable_frame_span_count_ = 0;
    std::uint64_t out_of_order_frame_count_ = 0;
    std::uint64_t duplicate_frame_id_count_ = 0;
    std::uint64_t last_received_frame_id_ = 0;
    bool has_last_received_frame_id_ = false;

    guint64 extended_rtp_timestamp_ = GST_CLOCK_TIME_NONE;
    bool clock_error_reported_ = false;
    bool pts_diagnostic_reported_ = false;

    bool opened_ = false;
};
