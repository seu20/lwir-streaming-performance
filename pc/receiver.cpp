#include "receiver.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>

#include <gst/rtp/gstrtpbuffer.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>


namespace
{
constexpr guint8 kE2eMetadataExtensionId = 1;
constexpr guint kE2eMetadataSize = 16;
constexpr std::size_t kMaxE2eMetrics = 10000;
constexpr std::size_t kMaxFpsMetrics = 10000;
constexpr std::size_t kMaxPtsTraces = 40000;
constexpr std::size_t kMaxPendingMetadata = 1000;
constexpr guint kRtpClockRate = 90000;
}


Receiver::Receiver(ReceiverConfig config)
    : port_(config.port),
      measurement_enabled_(config.measurement_enabled),
      clocks_synchronized_(config.clocks_synchronized),
      recording_enabled_(config.recording_enabled),
      output_path_(std::move(config.recording_output_path)),
      recording_fps_(config.recording_fps),
      metrics_output_dir_(std::move(config.metrics_output_dir)),
      idle_timeout_seconds_(config.idle_timeout_seconds),
      udp_buffer_bytes_(config.udp_buffer_bytes),
      jitter_latency_ms_(config.jitter_latency_ms)
{
}


Receiver::~Receiver()
{
    close();
}


bool Receiver::open()
{
    if (opened_)
        return true;

    if (!prepare_recording_output())
        return false;

    if (!metrics_output_dir_.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(
            metrics_output_dir_,
            error
        );
        if (error)
        {
            std::cerr
                << "[Receiver] Metrics 디렉터리 생성 실패: "
                << metrics_output_dir_ << " ("
                << error.message() << ")\n";
            return false;
        }
    }

    gst_init(nullptr, nullptr);

    /*
     * Raspberry Pi에서 전송한 RTP/H.264 수신 Pipeline
     *
     * udpsrc
     *   ↓ UDP packet 수신
     *
     * rtph264depay
     *   ↓ RTP header 제거 → H.264 stream 복원
     *
     * h264parse
     *   ↓ H.264 bitstream 정리
     *
     * avdec_h264
     *   ↓ H.264 → Raw Video decode
     *
     * videoconvert
     *   ↓ OpenCV가 받을 BGR로 변환
     *
     * appsink
     *   ↓ C++에서 Frame 수신
     */
    std::string source_options =
        "udpsrc name=receiver_source port=" +
        std::to_string(port_);

    if (udp_buffer_bytes_ > 0)
    {
        source_options +=
            " buffer-size=" +
            std::to_string(udp_buffer_bytes_);
    }

    std::string jitter_element;
    if (jitter_latency_ms_ >= 0)
    {
        jitter_element =
            "! rtpjitterbuffer latency=" +
            std::to_string(jitter_latency_ms_) +
            " drop-on-latency=true ";
    }

    const std::string pipeline_description =
        source_options +
        " caps=\"application/x-rtp,"
        "media=video,"
        "encoding-name=H264,"
        "payload=96,"
        "clock-rate=90000\" " +
        jitter_element +
        "! rtph264depay name=depayloader "
        "! h264parse name=parser "
        "! avdec_h264 name=decoder "
        "! videoconvert "
        "! video/x-raw,format=BGR "
        "! appsink name=receiver_sink "
        "sync=false "
        "max-buffers=1 "
        "drop=true";

    GError* error = nullptr;

    pipeline_ = gst_parse_launch(
        pipeline_description.c_str(),
        &error
    );

    if (!pipeline_)
    {
        std::cerr
            << "[Receiver] Pipeline 생성 실패";

        if (error)
        {
            std::cerr << ": " << error->message;
            g_error_free(error);
        }

        std::cerr << '\n';
        return false;
    }

    appsink_ = gst_bin_get_by_name(
        GST_BIN(pipeline_),
        "receiver_sink"
    );

    rtp_source_ = gst_bin_get_by_name(
        GST_BIN(pipeline_),
        "receiver_source"
    );

    depayloader_ = gst_bin_get_by_name(
        GST_BIN(pipeline_),
        "depayloader"
    );

    parser_ = gst_bin_get_by_name(
        GST_BIN(pipeline_),
        "parser"
    );

    decoder_ = gst_bin_get_by_name(
        GST_BIN(pipeline_),
        "decoder"
    );

    if (!appsink_ || !rtp_source_ ||
        !depayloader_ || !parser_ || !decoder_)
    {
        std::cerr
            << "[Receiver] Pipeline element를 찾을 수 없습니다\n";

        close();
        return false;
    }

    if (measurement_enabled_ &&
        !install_e2e_probes())
    {
        std::cerr
            << "[Receiver] E2E RTP probe 설치 실패\n";

        close();
        return false;
    }

    if (measurement_enabled_ &&
        !clocks_synchronized_)
    {
        std::cout
            << "[Receiver] 양쪽 시계 동기화 확인 없음: "
            << "E2E latency를 측정하지 않습니다\n";
    }

    /*
     * appsink 설정
     *
     * max-buffers=1:
     *   Application이 늦어져도 Frame을 계속 쌓지 않는다.
     *
     * drop=true:
     *   Buffer가 가득 차면 오래된 Frame을 버린다.
     *
     * sync=false:
     *   Sink clock에 맞춰 기다리지 않고 가능한 즉시 전달한다.
     *
     * PC Receiver에서 불필요한 Playback Latency를
     * 최소화하기 위한 설정이다.
     */
    gst_app_sink_set_max_buffers(
        GST_APP_SINK(appsink_),
        1
    );

    gst_app_sink_set_drop(
        GST_APP_SINK(appsink_),
        TRUE
    );

    const GstStateChangeReturn result =
        gst_element_set_state(
            pipeline_,
            GST_STATE_PLAYING
        );

    if (result == GST_STATE_CHANGE_FAILURE)
    {
        std::cerr
            << "[Receiver] PLAYING 상태 전환 실패\n";

        close();
        return false;
    }

    opened_ = true;

    std::cout
        << "[Receiver] UDP port "
        << port_
        << " 수신 시작\n";

    return true;
}


void Receiver::run()
{
    if (!opened_)
        return;

    cv::namedWindow(
        "LWIR Stream",
        cv::WINDOW_AUTOSIZE
    );

    if (measurement_enabled_)
    {
        fps_started_at_ =
            std::chrono::steady_clock::now();
        fps_interval_started_at_ =
            fps_started_at_;
    }

    bool received_any_frame = false;
    auto last_frame_received_at =
        std::chrono::steady_clock::now();

    while (true)
    {
        /*
         * 디코딩된 Raw Frame 하나를 appsink에서 가져온다.
         *
         * 100 ms timeout을 둬서 Stream이 잠깐 없어도
         * Thread가 영원히 block되지 않게 한다.
         */
        GstSample* sample =
            gst_app_sink_try_pull_sample(
                GST_APP_SINK(appsink_),
                100 * GST_MSECOND
            );

        if (!sample)
        {
            if (gst_app_sink_is_eos(
                    GST_APP_SINK(appsink_)))
            {
                break;
            }

            // 영상이 아직 도착하지 않은 경우에도
            // q / ESC 입력을 확인한다.
            const int key = cv::waitKey(1);

            if (key == 'q' || key == 27)
                break;

            if (received_any_frame &&
                idle_timeout_seconds_ > 0.0 &&
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() -
                    last_frame_received_at
                ).count() >= idle_timeout_seconds_)
            {
                std::cout
                    << "[Receiver] 입력 없음 timeout으로 종료\n";
                break;
            }

            continue;
        }

        // GstSample을 성공적으로 가져온 직후의 시각이다.
        // 이후 화면 출력 및 MP4 압축 시간은 포함하지 않는다.
        const auto received_at =
            std::chrono::steady_clock::now();
        received_any_frame = true;
        last_frame_received_at = received_at;

        GstCaps* caps =
            gst_sample_get_caps(sample);

        GstBuffer* buffer =
            gst_sample_get_buffer(sample);

        if (!caps || !buffer)
        {
            gst_sample_unref(sample);
            continue;
        }

        if (measurement_enabled_)
        {
            record_received_frame(
                buffer,
                std::chrono::steady_clock::now()
            );
        }

        const GstStructure* structure =
            gst_caps_get_structure(caps, 0);

        int width = 0;
        int height = 0;

        gst_structure_get_int(
            structure,
            "width",
            &width
        );

        gst_structure_get_int(
            structure,
            "height",
            &height
        );


        GstMapInfo map{};

        if (!gst_buffer_map(
                buffer,
                &map,
                GST_MAP_READ))
        {
            gst_sample_unref(sample);
            continue;
        }


        /*
         * GStreamer videoconvert 출력이 BGR이므로
         * OpenCV에서는 CV_8UC3로 해석한다.
         *
         * 이 Mat은 GstBuffer 메모리를 직접 바라본다.
         * 따라서 sample을 unref하기 전에 화면 처리까지 끝낸다.
         */
        cv::Mat frame(
            height,
            width,
            CV_8UC3,
            map.data
        );


        if (recording_enabled_ &&
            !recording_failed_)
        {
            record_frame(frame, received_at);
        }


        cv::imshow(
            "LWIR Stream",
            frame
        );


        const int key =
            cv::waitKey(1);


        gst_buffer_unmap(
            buffer,
            &map
        );

        gst_sample_unref(sample);


        if (key == 'q' || key == 27)
            break;
    }


    if (measurement_enabled_)
    {
        finish_fps_measurement(
            std::chrono::steady_clock::now()
        );
    }

    // VideoWriter flush 시간이 Receiver FPS 측정 구간에 들어가지 않도록
    // appsink 수신 측정을 먼저 종료한다.
    finish_recording();

    if (measurement_enabled_)
    {
        print_measurement_summary();
        save_fps_metrics_csv();
        save_frame_metrics_csv();
        save_pts_trace_csv();
    }

    cv::destroyAllWindows();

    if (measurement_enabled_ &&
        clocks_synchronized_)
    {
        save_e2e_metrics_csv();
    }
}


GstPadProbeReturn Receiver::rtp_probe(
    GstPad*,
    GstPadProbeInfo* info,
    gpointer user_data)
{
    auto* receiver =
        static_cast<Receiver*>(user_data);

    receiver->receive_rtp_metadata(info);

    return GST_PAD_PROBE_OK;
}


GstPadProbeReturn Receiver::appsink_probe(
    GstPad*,
    GstPadProbeInfo* info,
    gpointer user_data)
{
    auto* receiver =
        static_cast<Receiver*>(user_data);

    GstBuffer* buffer =
        GST_PAD_PROBE_INFO_BUFFER(info);

    if (buffer &&
        receiver->clocks_synchronized_)
    {
        // 디코딩 Frame이 appsink에 도착한 즉시 측정한다.
        receiver->record_e2e(
            buffer,
            std::chrono::system_clock::now()
        );
    }

    return GST_PAD_PROBE_OK;
}


GstPadProbeReturn Receiver::pts_trace_probe(
    GstPad* pad,
    GstPadProbeInfo* info,
    gpointer user_data)
{
    auto* receiver =
        static_cast<Receiver*>(user_data);

    GstBuffer* buffer =
        GST_PAD_PROBE_INFO_BUFFER(info);

    if (buffer && pad == receiver->depayloader_src_pad_)
    {
        buffer = receiver->identify_depayloaded_access_unit(
            buffer
        );

        if (buffer)
            GST_PAD_PROBE_INFO_DATA(info) = buffer;
    }

    if (buffer && pad == receiver->parser_src_pad_)
        receiver->associate_access_unit(buffer);

    if (buffer)
        receiver->record_pts_trace(pad, buffer);

    return GST_PAD_PROBE_OK;
}


GstBuffer* Receiver::identify_depayloaded_access_unit(
    GstBuffer* buffer)
{
    GstStructure* stats = nullptr;

    g_object_get(
        G_OBJECT(depayloader_),
        "stats", &stats,
        nullptr
    );

    if (!stats)
        return buffer;

    guint rtp_timestamp = 0;
    const gboolean has_timestamp =
        gst_structure_get_uint(
            stats,
            "timestamp",
            &rtp_timestamp
        );

    gst_structure_free(stats);

    if (!has_timestamp)
        return buffer;

    std::lock_guard<std::mutex> lock(
        metrics_mutex_
    );

    const auto metadata =
        rtp_metadata_.find(rtp_timestamp);

    if (metadata == rtp_metadata_.end())
        return buffer;

    const guint64 extended_timestamp =
        gst_rtp_buffer_ext_timestamp(
            &extended_rtp_timestamp_,
            rtp_timestamp
        );

    buffer = gst_buffer_make_writable(buffer);

    if (!buffer)
        return nullptr;

    /*
     * udpsrc의 도착 시각이 아니라 RTP의 90 kHz presentation
     * timestamp를 완성된 H.264 access unit에 직접 부여한다.
     * Decode timestamp는 알 수 없으므로 parser가 계산하게 둔다.
     */
    GST_BUFFER_PTS(buffer) =
        gst_util_uint64_scale(
            extended_timestamp,
            GST_SECOND,
            kRtpClockRate
        );
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;

    depayloaded_access_units_.push_back(
        metadata->second
    );

    rtp_metadata_.erase(metadata);

    return buffer;
}


void Receiver::associate_access_unit(GstBuffer* buffer)
{
    const GstClockTime pts =
        GST_BUFFER_PTS(buffer);

    std::lock_guard<std::mutex> lock(
        metrics_mutex_
    );

    if (depayloaded_access_units_.empty())
        return;

    const ReceivedMetadata metadata =
        depayloaded_access_units_.front();

    depayloaded_access_units_.pop_front();

    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return;

    const auto inserted =
        pending_metadata_.emplace(pts, metadata);

    if (!inserted.second)
        inserted.first->second = metadata;
    else
        pending_order_.push_back(pts);

    while (pending_order_.size() >
           kMaxPendingMetadata)
    {
        const GstClockTime oldest =
            pending_order_.front();

        pending_order_.pop_front();
        pending_metadata_.erase(oldest);
    }
}


void Receiver::record_pts_trace(
    GstPad* pad,
    GstBuffer* buffer)
{
    const char* stage = nullptr;
    std::uint64_t* sequence = nullptr;

    if (pad == depayloader_src_pad_)
    {
        stage = "depay_src";
        sequence = &depayloader_src_sequence_;
    }
    else if (pad == parser_src_pad_)
    {
        stage = "parser_src";
        sequence = &parser_src_sequence_;
    }
    else if (pad == decoder_sink_pad_)
    {
        stage = "decoder_sink";
        sequence = &decoder_sink_sequence_;
    }
    else if (pad == decoder_src_pad_)
    {
        stage = "decoder_src";
        sequence = &decoder_src_sequence_;
    }

    if (!stage || !sequence)
        return;

    std::lock_guard<std::mutex> lock(
        metrics_mutex_
    );

    ++(*sequence);

    if (pts_traces_.size() >= kMaxPtsTraces)
        return;

    pts_traces_.push_back({
        stage,
        *sequence,
        GST_BUFFER_PTS(buffer),
        GST_BUFFER_DTS(buffer),
        GST_BUFFER_DURATION(buffer)
    });
}


void Receiver::receive_rtp_metadata(
    GstPadProbeInfo* info)
{
    GstBuffer* buffer =
        GST_PAD_PROBE_INFO_BUFFER(info);

    if (!buffer)
        return;

    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;

    if (!gst_rtp_buffer_map(
            buffer,
            GST_MAP_READ,
            &rtp))
    {
        return;
    }

    gpointer extension_data = nullptr;
    guint extension_size = 0;

    const gboolean has_metadata =
        gst_rtp_buffer_get_extension_onebyte_header(
            &rtp,
            kE2eMetadataExtensionId,
            0,
            &extension_data,
            &extension_size
        );

    if (!has_metadata ||
        extension_size != kE2eMetadataSize)
    {
        gst_rtp_buffer_unmap(&rtp);
        return;
    }

    guint64 frame_id_be = 0;
    guint64 captured_ns_be = 0;

    std::memcpy(
        &frame_id_be,
        extension_data,
        sizeof(frame_id_be)
    );

    std::memcpy(
        &captured_ns_be,
        static_cast<guint8*>(extension_data) +
            sizeof(frame_id_be),
        sizeof(captured_ns_be)
    );

    const std::uint64_t frame_id =
        GUINT64_FROM_BE(frame_id_be);

    const std::int64_t captured_system_ns =
        static_cast<std::int64_t>(
            GUINT64_FROM_BE(captured_ns_be)
        );

    const guint32 rtp_timestamp =
        gst_rtp_buffer_get_timestamp(&rtp);

    gst_rtp_buffer_unmap(&rtp);

    if (frame_id == 0)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(
            metrics_mutex_
        );

        const auto inserted =
            rtp_metadata_.emplace(
                rtp_timestamp,
                ReceivedMetadata{
                    frame_id,
                    captured_system_ns
                }
            );

        if (!inserted.second)
        {
            inserted.first->second = {
                frame_id,
                captured_system_ns
            };
        }
        else
        {
            rtp_metadata_order_.push_back(
                rtp_timestamp
            );
        }

        while (rtp_metadata_order_.size() >
               kMaxPendingMetadata)
        {
            const guint32 oldest =
                rtp_metadata_order_.front();

            rtp_metadata_order_.pop_front();
            rtp_metadata_.erase(oldest);
        }
    }
}


std::unordered_map<
    GstClockTime,
    Receiver::ReceivedMetadata>::iterator
Receiver::find_metadata_for_decoder_pts(
    GstClockTime decoder_pts)
{
    if (pending_metadata_.empty())
        return pending_metadata_.end();

    while (!pending_order_.empty() &&
           pending_metadata_.find(
               pending_order_.front()
           ) == pending_metadata_.end())
    {
        pending_order_.pop_front();
    }

    auto exact = pending_metadata_.find(decoder_pts);
    if (exact != pending_metadata_.end())
        return exact;

    constexpr GstClockTime kMatchTolerance = GST_MSECOND;
    auto nearest = pending_metadata_.end();
    GstClockTime nearest_distance = GST_CLOCK_TIME_NONE;

    for (auto candidate = pending_metadata_.begin();
         candidate != pending_metadata_.end();
         ++candidate)
    {
        const GstClockTime distance =
            candidate->first > decoder_pts
                ? candidate->first - decoder_pts
                : decoder_pts - candidate->first;

        if (distance < nearest_distance)
        {
            nearest = candidate;
            nearest_distance = distance;
        }
    }

    if (nearest != pending_metadata_.end() &&
        nearest_distance <= kMatchTolerance)
    {
        return nearest;
    }

    if (!pts_diagnostic_reported_)
    {
        std::cerr
            << "[Receiver] PTS match 실패 decoder="
            << decoder_pts
            << " nearest_delta=" << nearest_distance
            << '\n';
        pts_diagnostic_reported_ = true;
    }

    return pending_metadata_.end();
}


void Receiver::record_e2e(
    GstBuffer* buffer,
    std::chrono::system_clock::time_point received_at)
{
    if (!clocks_synchronized_)
        return;

    const GstClockTime pts =
        GST_BUFFER_PTS(buffer);

    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return;

    const std::int64_t received_ns =
        std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                received_at.time_since_epoch()
            ).count();

    std::lock_guard<std::mutex> lock(
        metrics_mutex_
    );

    auto matched_metadata =
        find_metadata_for_decoder_pts(pts);

    if (matched_metadata == pending_metadata_.end())
        return;

    if (matched_metadata->second.captured_system_ns <= 0)
        return;

    const std::int64_t latency_ns =
        received_ns -
        matched_metadata->second.captured_system_ns;

    if (latency_ns < 0)
    {
        if (!clock_error_reported_)
        {
            std::cerr
                << "[Receiver] 음수 E2E latency 감지: "
                << "시계 동기화를 다시 확인하십시오\n";

            clock_error_reported_ = true;
        }

        return;
    }

    if (e2e_metrics_.size() < kMaxE2eMetrics)
    {
        e2e_metrics_.push_back({
            matched_metadata->second.frame_id,
            static_cast<double>(latency_ns) /
                1000000.0
        });
    }

}


void Receiver::record_received_frame(
    GstBuffer* buffer,
    std::chrono::steady_clock::time_point received_at)
{
    constexpr auto interval =
        std::chrono::seconds(1);

    while (received_at -
               fps_interval_started_at_ >= interval)
    {
        const auto interval_finished_at =
            fps_interval_started_at_ + interval;

        if (fps_metrics_.size() < kMaxFpsMetrics)
        {
            FpsMetric metric;
            metric.interval_start_s =
                std::chrono::duration<double>(
                    fps_interval_started_at_ -
                    fps_started_at_
                ).count();
            metric.interval_end_s =
                std::chrono::duration<double>(
                    interval_finished_at -
                    fps_started_at_
                ).count();
            metric.frame_count =
                fps_interval_frame_count_;
            metric.fps =
                static_cast<double>(
                    fps_interval_frame_count_
                );

            fps_metrics_.push_back(metric);
        }

        fps_interval_started_at_ =
            interval_finished_at;
        fps_interval_frame_count_ = 0;
    }

    ++fps_interval_frame_count_;
    ++decoded_frame_count_;

    const GstClockTime pts =
        GST_BUFFER_PTS(buffer);

    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return;

    std::lock_guard<std::mutex> lock(
        metrics_mutex_
    );

    auto metadata =
        find_metadata_for_decoder_pts(pts);

    if (metadata == pending_metadata_.end())
        return;

    const std::uint64_t frame_id =
        metadata->second.frame_id;

    ++metadata_matched_frame_count_;

    if (!has_last_received_frame_id_)
    {
        last_received_frame_id_ = frame_id;
        observable_frame_span_count_ = 1;
        has_last_received_frame_id_ = true;
    }
    else if (frame_id > last_received_frame_id_)
    {
        const std::uint64_t advance =
            frame_id - last_received_frame_id_;

        if (advance > 1)
            missing_frame_id_count_ += advance - 1;

        observable_frame_span_count_ += advance;
        last_received_frame_id_ = frame_id;
    }
    else if (frame_id == last_received_frame_id_)
    {
        ++duplicate_frame_id_count_;
    }
    else
    {
        ++out_of_order_frame_count_;
    }

    const GstClockTime matched_pts = metadata->first;
    pending_metadata_.erase(metadata);

    if (!pending_order_.empty() &&
        pending_order_.front() == matched_pts)
    {
        pending_order_.pop_front();
    }
}


void Receiver::finish_fps_measurement(
    std::chrono::steady_clock::time_point finished_at)
{
    constexpr auto interval =
        std::chrono::seconds(1);

    while (finished_at -
               fps_interval_started_at_ >= interval)
    {
        const auto interval_finished_at =
            fps_interval_started_at_ + interval;

        if (fps_metrics_.size() < kMaxFpsMetrics)
        {
            FpsMetric metric;
            metric.interval_start_s =
                std::chrono::duration<double>(
                    fps_interval_started_at_ -
                    fps_started_at_
                ).count();
            metric.interval_end_s =
                std::chrono::duration<double>(
                    interval_finished_at -
                    fps_started_at_
                ).count();
            metric.frame_count =
                fps_interval_frame_count_;
            metric.fps =
                static_cast<double>(
                    fps_interval_frame_count_
                );

            fps_metrics_.push_back(metric);
        }

        fps_interval_started_at_ =
            interval_finished_at;
        fps_interval_frame_count_ = 0;
    }

    const double partial_seconds =
        std::chrono::duration<double>(
            finished_at - fps_interval_started_at_
        ).count();

    if (partial_seconds > 0.0 &&
        fps_metrics_.size() < kMaxFpsMetrics)
    {
        FpsMetric metric;
        metric.interval_start_s =
            std::chrono::duration<double>(
                fps_interval_started_at_ -
                fps_started_at_
            ).count();
        metric.interval_end_s =
            std::chrono::duration<double>(
                finished_at - fps_started_at_
            ).count();
        metric.frame_count =
            fps_interval_frame_count_;
        metric.fps =
            static_cast<double>(
                fps_interval_frame_count_
            ) / partial_seconds;

        fps_metrics_.push_back(metric);
    }

    fps_duration_seconds_ =
        std::chrono::duration<double>(
            finished_at - fps_started_at_
        ).count();
    fps_interval_frame_count_ = 0;
}


void Receiver::save_e2e_metrics_csv() const
{
    std::vector<E2eMetric> metrics;

    {
        std::lock_guard<std::mutex> lock(
            metrics_mutex_
        );

        metrics = e2e_metrics_;
    }

    std::ofstream file(
        metric_path("receiver_e2e_metrics.csv")
    );

    if (!file.is_open())
    {
        std::cerr
            << "[Receiver] E2E CSV 열기 실패\n";
        return;
    }

    file << "frame_id,e2e_ms\n";
    file << std::fixed << std::setprecision(3);

    for (const auto& metric : metrics)
    {
        file
            << metric.frame_id << ","
            << metric.e2e_ms << "\n";
    }
}


void Receiver::save_fps_metrics_csv() const
{
    std::ofstream file(
        metric_path("receiver_fps_metrics.csv")
    );

    if (!file.is_open())
    {
        std::cerr
            << "[Receiver] FPS CSV 열기 실패\n";
        return;
    }

    file
        << "interval_index,interval_start_s,"
        << "interval_end_s,frame_count,fps\n";
    file << std::fixed << std::setprecision(3);

    for (std::size_t index = 0;
         index < fps_metrics_.size();
         ++index)
    {
        const auto& metric = fps_metrics_[index];

        file
            << index + 1 << ","
            << metric.interval_start_s << ","
            << metric.interval_end_s << ","
            << metric.frame_count << ","
            << metric.fps << "\n";
    }
}


void Receiver::save_frame_metrics_csv() const
{
    std::uint64_t matched_frame_count = 0;
    std::uint64_t missing_frame_count = 0;
    std::uint64_t observable_span_count = 0;
    std::uint64_t out_of_order_count = 0;
    std::uint64_t duplicate_count = 0;

    {
        std::lock_guard<std::mutex> lock(
            metrics_mutex_
        );

        matched_frame_count =
            metadata_matched_frame_count_;
        missing_frame_count =
            missing_frame_id_count_;
        observable_span_count =
            observable_frame_span_count_;
        out_of_order_count =
            out_of_order_frame_count_;
        duplicate_count =
            duplicate_frame_id_count_;
    }

    const double observable_drop_rate =
        observable_span_count > 0
            ? 100.0 *
                static_cast<double>(missing_frame_count) /
                static_cast<double>(observable_span_count)
            : 0.0;

    std::ofstream file(
        metric_path("receiver_frame_metrics.csv")
    );

    if (!file.is_open())
    {
        std::cerr
            << "[Receiver] Frame CSV 열기 실패\n";
        return;
    }

    file
        << "decoded_frame_count,"
        << "metadata_matched_frame_count,"
        << "metadata_unmatched_frame_count,"
        << "observable_frame_span_count,"
        << "missing_frame_id_count,"
        << "observable_drop_rate_pct,"
        << "out_of_order_frame_count,"
        << "duplicate_frame_id_count\n";

    file << std::fixed << std::setprecision(3);
    file
        << decoded_frame_count_ << ","
        << matched_frame_count << ","
        << decoded_frame_count_ -
            matched_frame_count << ","
        << observable_span_count << ","
        << missing_frame_count << ","
        << observable_drop_rate << ","
        << out_of_order_count << ","
        << duplicate_count << "\n";
}


void Receiver::save_pts_trace_csv() const
{
    std::vector<PtsTrace> traces;

    {
        std::lock_guard<std::mutex> lock(
            metrics_mutex_
        );

        traces = pts_traces_;
    }

    std::ofstream file(
        metric_path("receiver_pts_trace.csv")
    );

    if (!file.is_open())
    {
        std::cerr
            << "[Receiver] PTS trace CSV 열기 실패\n";
        return;
    }

    file << "stage,sequence,pts_ns,dts_ns,duration_ns\n";

    for (const auto& trace : traces)
    {
        file << trace.stage << ","
             << trace.sequence << ",";

        if (GST_CLOCK_TIME_IS_VALID(trace.pts))
            file << trace.pts;

        file << ",";

        if (GST_CLOCK_TIME_IS_VALID(trace.dts))
            file << trace.dts;

        file << ",";

        if (GST_CLOCK_TIME_IS_VALID(trace.duration))
            file << trace.duration;

        file << "\n";
    }
}


void Receiver::print_measurement_summary() const
{
    const double average_fps =
        fps_duration_seconds_ > 0.0
            ? static_cast<double>(
                  decoded_frame_count_
              ) / fps_duration_seconds_
            : 0.0;

    std::uint64_t missing_frame_count = 0;
    std::uint64_t observable_span_count = 0;

    {
        std::lock_guard<std::mutex> lock(
            metrics_mutex_
        );

        missing_frame_count =
            missing_frame_id_count_;
        observable_span_count =
            observable_frame_span_count_;
    }

    const double observable_drop_rate =
        observable_span_count > 0
            ? 100.0 *
                static_cast<double>(missing_frame_count) /
                static_cast<double>(observable_span_count)
            : 0.0;

    std::cout
        << std::fixed
        << std::setprecision(3)
        << "[Receiver] decoded frames="
        << decoded_frame_count_
        << " duration=" << fps_duration_seconds_ << " s"
        << " average=" << average_fps << " FPS\n"
        << "[Receiver] observable missing frame IDs="
        << missing_frame_count
        << " denominator=" << observable_span_count
        << " drop_rate=" << observable_drop_rate
        << "%\n";
}


std::string Receiver::metric_path(const char* filename) const
{
    if (metrics_output_dir_.empty())
        return filename;

    return (
        std::filesystem::path(metrics_output_dir_) /
        filename
    ).string();
}


bool Receiver::prepare_recording_output()
{
    if (!recording_enabled_)
        return true;

    if (output_path_.empty())
    {
        std::cerr
            << "[Receiver] MP4 출력 경로가 비어 있습니다\n";
        return false;
    }

    if (recording_fps_ <= 0.0)
    {
        std::cerr
            << "[Receiver] recording_fps는 0보다 커야 합니다\n";
        return false;
    }

    const std::filesystem::path output_path(
        output_path_
    );

    if (output_path.filename().empty())
    {
        std::cerr
            << "[Receiver] MP4 파일명이 유효하지 않습니다: "
            << output_path_ << '\n';
        return false;
    }

    const std::filesystem::path parent_path =
        output_path.parent_path();
    std::error_code error;

    if (!parent_path.empty())
    {
        std::filesystem::create_directories(
            parent_path,
            error
        );

        if (error)
        {
            std::cerr
                << "[Receiver] 녹화 디렉터리 생성 실패: "
                << parent_path.string()
                << " (" << error.message() << ")\n";
            return false;
        }
    }

    timestamp_output_path_ =
        (parent_path /
         (output_path.stem().string() +
          "_timestamps.csv")).string();
    recording_prepared_ = true;

    std::cout
        << "[Receiver] 녹화 활성화: "
        << output_path_
        << " / " << recording_fps_
        << " FPS / codec=mp4v\n";

    return true;
}


bool Receiver::initialize_video_writer(
    const cv::Size& frame_size)
{
    if (recording_initialized_)
    {
        if (frame_size == recording_frame_size_)
            return true;

        std::cerr
            << "[Receiver] 녹화 중 Frame 해상도가 변경되었습니다\n";
        recording_failed_ = true;
        return false;
    }

    if (frame_size.width <= 0 ||
        frame_size.height <= 0)
    {
        std::cerr
            << "[Receiver] 녹화 Frame 해상도가 유효하지 않습니다\n";
        recording_failed_ = true;
        return false;
    }

    const int fourcc = cv::VideoWriter::fourcc(
        'm', 'p', '4', 'v'
    );

    try
    {
        video_writer_.open(
            output_path_,
            fourcc,
            recording_fps_,
            frame_size,
            true
        );
    }
    catch (const cv::Exception& exception)
    {
        std::cerr
            << "[Receiver] VideoWriter 초기화 예외: "
            << exception.what() << '\n';
        recording_failed_ = true;
        return false;
    }

    if (!video_writer_.isOpened())
    {
        std::cerr
            << "[Receiver] MP4 VideoWriter를 열지 못했습니다. "
            << "OpenCV의 mp4v 지원 여부와 출력 경로를 확인하십시오\n";
        recording_failed_ = true;
        return false;
    }

    recording_frame_size_ = frame_size;
    recording_initialized_ = true;
    return true;
}


bool Receiver::record_frame(
    const cv::Mat& frame,
    std::chrono::steady_clock::time_point received_at)
{
    if (!recording_enabled_ ||
        recording_failed_)
    {
        return false;
    }

    if (frame.empty() || frame.type() != CV_8UC3)
    {
        std::cerr
            << "[Receiver] 녹화 Frame이 유효한 BGR 형식이 아닙니다\n";
        recording_failed_ = true;
        return false;
    }

    if (!initialize_video_writer(frame.size()))
        return false;

    try
    {
        // write()가 반환될 때까지 GstBuffer를 unmap하지 않으므로
        // VideoWriter가 유효한 BGR 메모리를 사용한다.
        video_writer_.write(frame);
    }
    catch (const cv::Exception& exception)
    {
        std::cerr
            << "[Receiver] MP4 Frame 저장 실패: "
            << exception.what() << '\n';
        recording_failed_ = true;
        return false;
    }

    if (recording_timestamps_.empty())
        recording_started_at_ = received_at;

    RecordingTimestamp timestamp;
    timestamp.frame_index =
        recording_timestamps_.size() + 1;
    timestamp.received_at_ms =
        std::chrono::duration<double, std::milli>(
            received_at - recording_started_at_
        ).count();

    // VideoWriter에 정상 제출된 Frame만 CSV와 1:1로 대응한다.
    recording_timestamps_.push_back(timestamp);
    return true;
}


void Receiver::save_recording_timestamps_csv() const
{
    std::ofstream file(timestamp_output_path_);

    if (!file.is_open())
    {
        std::cerr
            << "[Receiver] 녹화 Timestamp CSV 열기 실패: "
            << timestamp_output_path_ << '\n';
        return;
    }

    file << "frame_index,received_at_ms\n";
    file << std::fixed << std::setprecision(3);

    for (const auto& timestamp : recording_timestamps_)
    {
        file
            << timestamp.frame_index << ","
            << timestamp.received_at_ms << "\n";
    }

    if (!file.good())
    {
        std::cerr
            << "[Receiver] 녹화 Timestamp CSV 저장 실패: "
            << timestamp_output_path_ << '\n';
    }
}


void Receiver::finish_recording()
{
    if (!recording_enabled_ ||
        !recording_prepared_ ||
        recording_finished_)
    {
        return;
    }

    if (video_writer_.isOpened())
    {
        try
        {
            video_writer_.release();
        }
        catch (const cv::Exception& exception)
        {
            std::cerr
                << "[Receiver] VideoWriter 종료 실패: "
                << exception.what() << '\n';
            recording_failed_ = true;
        }
    }

    if (recording_initialized_)
    {
        std::error_code error;
        const bool output_exists =
            std::filesystem::exists(output_path_, error);
        const auto output_size =
            output_exists && !error
                ? std::filesystem::file_size(
                      output_path_,
                      error
                  )
                : 0;

        if (error || !output_exists || output_size == 0)
        {
            std::cerr
                << "[Receiver] MP4 파일 생성 확인 실패: "
                << output_path_ << '\n';
            recording_failed_ = true;
        }
    }

    save_recording_timestamps_csv();
    recording_finished_ = true;

    std::cout
        << "[Receiver] 녹화 종료: frames="
        << recording_timestamps_.size()
        << " mp4=" << output_path_
        << " timestamps=" << timestamp_output_path_
        << '\n';
}


bool Receiver::install_e2e_probes()
{
    rtp_source_pad_ =
        gst_element_get_static_pad(
            rtp_source_,
            "src"
        );

    appsink_sink_pad_ =
        gst_element_get_static_pad(
            appsink_,
            "sink"
        );

    depayloader_src_pad_ =
        gst_element_get_static_pad(
            depayloader_,
            "src"
        );

    parser_src_pad_ =
        gst_element_get_static_pad(
            parser_,
            "src"
        );

    decoder_sink_pad_ =
        gst_element_get_static_pad(
            decoder_,
            "sink"
        );

    decoder_src_pad_ =
        gst_element_get_static_pad(
            decoder_,
            "src"
        );

    if (!rtp_source_pad_ ||
        !appsink_sink_pad_ ||
        !depayloader_src_pad_ ||
        !parser_src_pad_ ||
        !decoder_sink_pad_ ||
        !decoder_src_pad_)
    {
        remove_e2e_probes();
        return false;
    }

    rtp_probe_id_ =
        gst_pad_add_probe(
            rtp_source_pad_,
            GST_PAD_PROBE_TYPE_BUFFER,
            &Receiver::rtp_probe,
            this,
            nullptr
        );

    appsink_probe_id_ =
        gst_pad_add_probe(
            appsink_sink_pad_,
            GST_PAD_PROBE_TYPE_BUFFER,
            &Receiver::appsink_probe,
            this,
            nullptr
        );

    depayloader_src_probe_id_ =
        gst_pad_add_probe(
            depayloader_src_pad_,
            GST_PAD_PROBE_TYPE_BUFFER,
            &Receiver::pts_trace_probe,
            this,
            nullptr
        );

    parser_src_probe_id_ =
        gst_pad_add_probe(
            parser_src_pad_,
            GST_PAD_PROBE_TYPE_BUFFER,
            &Receiver::pts_trace_probe,
            this,
            nullptr
        );

    decoder_sink_probe_id_ =
        gst_pad_add_probe(
            decoder_sink_pad_,
            GST_PAD_PROBE_TYPE_BUFFER,
            &Receiver::pts_trace_probe,
            this,
            nullptr
        );

    decoder_src_probe_id_ =
        gst_pad_add_probe(
            decoder_src_pad_,
            GST_PAD_PROBE_TYPE_BUFFER,
            &Receiver::pts_trace_probe,
            this,
            nullptr
        );

    if (rtp_probe_id_ == 0 ||
        appsink_probe_id_ == 0 ||
        depayloader_src_probe_id_ == 0 ||
        parser_src_probe_id_ == 0 ||
        decoder_sink_probe_id_ == 0 ||
        decoder_src_probe_id_ == 0)
    {
        remove_e2e_probes();
        return false;
    }

    return true;
}


void Receiver::remove_e2e_probes()
{
    if (rtp_source_pad_ &&
        rtp_probe_id_ != 0)
    {
        gst_pad_remove_probe(
            rtp_source_pad_,
            rtp_probe_id_
        );
    }

    if (appsink_sink_pad_ &&
        appsink_probe_id_ != 0)
    {
        gst_pad_remove_probe(
            appsink_sink_pad_,
            appsink_probe_id_
        );
    }

    if (depayloader_src_pad_ &&
        depayloader_src_probe_id_ != 0)
    {
        gst_pad_remove_probe(
            depayloader_src_pad_,
            depayloader_src_probe_id_
        );
    }

    if (parser_src_pad_ &&
        parser_src_probe_id_ != 0)
    {
        gst_pad_remove_probe(
            parser_src_pad_,
            parser_src_probe_id_
        );
    }

    if (decoder_sink_pad_ &&
        decoder_sink_probe_id_ != 0)
    {
        gst_pad_remove_probe(
            decoder_sink_pad_,
            decoder_sink_probe_id_
        );
    }

    if (decoder_src_pad_ &&
        decoder_src_probe_id_ != 0)
    {
        gst_pad_remove_probe(
            decoder_src_pad_,
            decoder_src_probe_id_
        );
    }

    rtp_probe_id_ = 0;
    appsink_probe_id_ = 0;
    depayloader_src_probe_id_ = 0;
    parser_src_probe_id_ = 0;
    decoder_sink_probe_id_ = 0;
    decoder_src_probe_id_ = 0;

    if (rtp_source_pad_)
    {
        gst_object_unref(rtp_source_pad_);
        rtp_source_pad_ = nullptr;
    }

    if (appsink_sink_pad_)
    {
        gst_object_unref(appsink_sink_pad_);
        appsink_sink_pad_ = nullptr;
    }

    if (depayloader_src_pad_)
    {
        gst_object_unref(depayloader_src_pad_);
        depayloader_src_pad_ = nullptr;
    }

    if (parser_src_pad_)
    {
        gst_object_unref(parser_src_pad_);
        parser_src_pad_ = nullptr;
    }

    if (decoder_sink_pad_)
    {
        gst_object_unref(decoder_sink_pad_);
        decoder_sink_pad_ = nullptr;
    }

    if (decoder_src_pad_)
    {
        gst_object_unref(decoder_src_pad_);
        decoder_src_pad_ = nullptr;
    }
}


void Receiver::close()
{
    finish_recording();

    if (pipeline_)
    {
        gst_element_set_state(
            pipeline_,
            GST_STATE_NULL
        );
    }

    remove_e2e_probes();

    if (appsink_)
    {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
    }

    if (rtp_source_)
    {
        gst_object_unref(rtp_source_);
        rtp_source_ = nullptr;
    }

    if (depayloader_)
    {
        gst_object_unref(depayloader_);
        depayloader_ = nullptr;
    }

    if (parser_)
    {
        gst_object_unref(parser_);
        parser_ = nullptr;
    }

    if (decoder_)
    {
        gst_object_unref(decoder_);
        decoder_ = nullptr;
    }

    if (pipeline_)
    {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }

    opened_ = false;

    {
        std::lock_guard<std::mutex> lock(
            metrics_mutex_
        );

        pending_metadata_.clear();
        pending_order_.clear();
        rtp_metadata_.clear();
        rtp_metadata_order_.clear();
        depayloaded_access_units_.clear();
        pts_traces_.clear();
        extended_rtp_timestamp_ = GST_CLOCK_TIME_NONE;
    }
}
