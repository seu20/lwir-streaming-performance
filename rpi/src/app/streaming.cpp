#include "app/streaming.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <memory>
#include <utility>

#include <gst/rtp/gstrtpbuffer.h>

#include "common/logger.hpp"


namespace
{
// 이 애플리케이션에서 사용하는 RTP one-byte header extension ID.
constexpr guint8 kE2eMetadataExtensionId = 1;
constexpr std::size_t kE2eMetadataSize = 16;

std::int64_t to_unix_nanoseconds(
    std::chrono::system_clock::time_point time_point)
{
    if (time_point ==
        std::chrono::system_clock::time_point{})
    {
        return 0;
    }

    return std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            time_point.time_since_epoch()
        ).count();
}
}


Streaming::Streaming(
    StreamingConfig config,
    bool measurement_enabled)
    : config_(std::move(config)),
      measurement_enabled_(measurement_enabled)
{
}


Streaming::~Streaming()
{
    close();
}


bool Streaming::open()
{
    if (opened_)
        return true;

    gst_init(nullptr, nullptr);

    pipeline_  = gst_pipeline_new("lwir-stream");
    appsrc_    = gst_element_factory_make("appsrc", "source");
    queue_     = gst_element_factory_make("queue", "queue");
    converter_ = gst_element_factory_make("videoconvert", "converter");
    encoder_capsfilter_ =
        gst_element_factory_make("capsfilter", "encoder-caps");
    encoder_   = gst_element_factory_make(
        config_.encoder_factory.c_str(),
        "encoder"
    );
    parser_    = gst_element_factory_make("h264parse", "parser");
    payloader_ = gst_element_factory_make("rtph264pay", "payloader");
    sink_      = gst_element_factory_make("udpsink", "sink");

    if (!pipeline_ || !appsrc_ || !queue_ ||
        !converter_ || !encoder_capsfilter_ ||
        !encoder_ || !parser_ || !payloader_ || !sink_)
    {
        Logger::error("[Streaming] GStreamer element 생성 실패");
        return false;
    }

    // Preprocess 출력 형식:
    // 640x480 / GRAY8 / 30 FPS
    //
    // appsrc까지는 전처리된 GRAY8 형식을 유지한다.
    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "GRAY8",
        "width", G_TYPE_INT, config_.width,
        "height", G_TYPE_INT, config_.height,
        "framerate", GST_TYPE_FRACTION, config_.fps, 1,
        nullptr
    );

    // appsrc:
    // Application이 만든 Frame을 GStreamer Pipeline에 넣는 입력 지점.
    //
    // is-live=true:
    // 파일 재생이 아니라 실시간 Camera Stream으로 취급.
    //
    // format=TIME:
    // Frame timestamp를 시간 단위로 처리.
    //
    // do-timestamp=true:
    // appsrc에 Frame이 들어오는 시점 기준으로 timestamp 자동 생성.
    g_object_set(
        G_OBJECT(appsrc_),
        "caps", caps,
        "is-live", TRUE,
        "format", GST_FORMAT_TIME,
        "do-timestamp", TRUE,
        nullptr
    );

    gst_caps_unref(caps);


    // Baseline은 I420 변환 경로를 사용한다. direct_gray8 실험에서는
    // 현재 x264enc가 지원하는 GRAY8을 직접 전달한다.
    // 영상은 흑백 그대로이며 U/V 평면에는 중립 색상값이 들어간다.
    GstCaps* encoder_caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING,
            config_.direct_gray8 ? "GRAY8" : "I420",
        "width", G_TYPE_INT, config_.width,
        "height", G_TYPE_INT, config_.height,
        "framerate", GST_TYPE_FRACTION, config_.fps, 1,
        nullptr
    );

    g_object_set(
        G_OBJECT(encoder_capsfilter_),
        "caps", encoder_caps,
        nullptr
    );

    gst_caps_unref(encoder_caps);


    // H.264 Encoder: x264enc
    //
    // 현재는 BASELINE이므로 별도 최적화 옵션을 지정하지 않는다.
    // 즉 x264enc 기본 preset / buffering / B-frame 정책을 사용한다.
    //
    // 이후 성능 실험에서 다음 옵션을 하나씩 비교한다.
    //
    // tune=zerolatency
    //   → Encoder 내부 buffering 감소
    //
    // speed-preset=ultrafast
    //   → 압축 효율보다 Encoding 속도 우선
    //
    // bframes=0
    //   → B-frame 제거로 추가 buffering 감소
    //
    // bitrate
    //   → 영상 품질 ↔ Network bandwidth trade-off
    //
    // Empty/-1 값은 원래 element 기본값을 보존한다.
    if (!config_.tune.empty())
    {
        gst_util_set_object_arg(
            G_OBJECT(encoder_),
            "tune",
            config_.tune.c_str()
        );
    }

    if (!config_.speed_preset.empty())
    {
        gst_util_set_object_arg(
            G_OBJECT(encoder_),
            "speed-preset",
            config_.speed_preset.c_str()
        );
    }

    if (config_.bframes >= 0)
    {
        g_object_set(
            G_OBJECT(encoder_),
            "bframes", config_.bframes,
            nullptr
        );
    }

    if (config_.bitrate_kbps > 0)
    {
        g_object_set(
            G_OBJECT(encoder_),
            "bitrate", config_.bitrate_kbps,
            nullptr
        );
    }

    if (config_.key_int_max >= 0)
    {
        g_object_set(
            G_OBJECT(encoder_),
            "key-int-max", config_.key_int_max,
            nullptr
        );
    }

    if (config_.gst_queue_depth >= 0)
    {
        g_object_set(
            G_OBJECT(queue_),
            "max-size-buffers", config_.gst_queue_depth,
            "max-size-bytes", 0,
            "max-size-time", static_cast<guint64>(0),
            nullptr
        );
    }

    g_object_set(
        G_OBJECT(queue_),
        "leaky", config_.gst_queue_leaky,
        nullptr
    );


    // H.264 bitstream을 RTP packet으로 변환.
    //
    // pt=96:
    // Dynamic RTP Payload Type.
    //
    // config-interval=-1:
    // IDR frame마다 SPS/PPS를 함께 전송하여
    // Receiver가 Stream 정보를 다시 얻기 쉽게 한다.
    g_object_set(
        G_OBJECT(payloader_),
        "pt", 96,
        "config-interval", -1,
        nullptr
    );


    // RTP/H.264 packet을 UDP로 PC에 전송.
    g_object_set(
        G_OBJECT(sink_),
        "host", config_.host.c_str(),
        "port", config_.port,
        nullptr
    );


    // 실제 GStreamer Pipeline:
    //
    // GRAY8 Frame
    //   → appsrc
    //   → queue
    //   → videoconvert   : GRAY8 → I420
    //   → capsfilter     : x264enc 입력 형식 고정
    //   → x264enc       : H.264 Encoding
    //   → h264parse     : H.264 bitstream 정리
    //   → rtph264pay    : RTP packet 생성
    //   → udpsink       : UDP 전송
    gst_bin_add_many(
        GST_BIN(pipeline_),
        appsrc_,
        queue_,
        converter_,
        encoder_capsfilter_,
        encoder_,
        parser_,
        payloader_,
        sink_,
        nullptr
    );

    const bool linked = config_.direct_gray8
        ? gst_element_link_many(
              appsrc_,
              queue_,
              encoder_capsfilter_,
              encoder_,
              parser_,
              payloader_,
              sink_,
              nullptr)
        : gst_element_link_many(
              appsrc_,
              queue_,
              converter_,
              encoder_capsfilter_,
              encoder_,
              parser_,
              payloader_,
              sink_,
              nullptr);

    if (!linked)
    {
        Logger::error("[Streaming] Pipeline link 실패");
        return false;
    }

    if (measurement_enabled_ &&
        !install_encoding_probes())
    {
        Logger::error(
            "[Streaming] Encoder latency probe 설치 실패"
        );
        return false;
    }

    if (gst_element_set_state(
            pipeline_,
            GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        Logger::error("[Streaming] PLAYING 전환 실패");
        return false;
    }

    opened_ = true;

    Logger::info("[Streaming] 시작");

    return true;
}


bool Streaming::push(const Frame& frame)
{
    if (!opened_ || !appsrc_)
        return false;

    if (frame.image.empty() ||
        frame.image.type() != CV_8UC1)
        return false;


    // GRAY8이므로 pixel 하나 = 1 byte.
    const std::size_t row_bytes =
        static_cast<std::size_t>(frame.image.cols);

    const std::size_t buffer_size =
        row_bytes *
        static_cast<std::size_t>(frame.image.rows);


    // GStreamer가 사용할 Frame Buffer 생성.
    GstBuffer* buffer = nullptr;

    if (config_.zero_copy_submit && frame.image.isContinuous())
    {
        auto* image_owner = new cv::Mat(frame.image);
        buffer = gst_buffer_new_wrapped_full(
            GST_MEMORY_FLAG_READONLY,
            image_owner->data,
            buffer_size,
            0,
            buffer_size,
            image_owner,
            [](gpointer data)
            {
                delete static_cast<cv::Mat*>(data);
            }
        );
    }
    else
    {
        buffer = gst_buffer_new_allocate(
            nullptr,
            buffer_size,
            nullptr
        );
    }

    if (!buffer)
        return false;


    if (!(config_.zero_copy_submit && frame.image.isContinuous()))
    {
        GstMapInfo map{};

        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE))
        {
            gst_buffer_unref(buffer);
            return false;
        }

        // cv::Mat의 step/padding 가능성을 고려해서 row 단위로 복사한다.
        for (int row = 0; row < frame.image.rows; ++row)
        {
            std::memcpy(
                map.data + row * row_bytes,
                frame.image.ptr<unsigned char>(row),
                row_bytes
            );
        }

        gst_buffer_unmap(buffer, &map);
    }


    // Frame 하나의 재생 시간.
    // 30 FPS라면 약 33.3 ms.
    GST_BUFFER_DURATION(buffer) =
        gst_util_uint64_scale_int(
            1,
            GST_SECOND,
            config_.fps
        );


    if (measurement_enabled_)
    {
        /*
         * do-timestamp=true는 PTS가 없는 Buffer에 appsrc가 실행 시각을
         * 부여한다. Frame ID와 Encoder 출력을 정확히 연결하기 위해
         * 측정 중에는 Frame ID 기반의 고유 PTS를 먼저 지정한다.
         * x264enc는 B-frame으로 출력 순서가 바뀌어도 Frame 사이의 PTS
         * 대응 관계를 유지하므로 출력 순서가 아닌 정규화 PTS로 찾는다.
         */
        const GstClockTime pts =
            gst_util_uint64_scale(
                frame.metadata.frame_id - 1,
                GST_SECOND,
                config_.fps
            );

        GST_BUFFER_PTS(buffer) = pts;

        std::lock_guard<std::mutex> lock(
            encoding_mutex_
        );

        pending_encodings_[pts] = {
            frame.metadata.frame_id,
            to_unix_nanoseconds(
                frame.metadata.captured_system_at
            ),
            {}
        };
    }


    // GstBuffer를 GStreamer 내부 Pipeline으로 전달.
    //
    // 여기서 반환되었다고 H.264 Encoding이 끝난 것은 아니다.
    // appsrc 내부 Queue에 Frame을 전달한 것뿐이다.
    const GstFlowReturn result =
        gst_app_src_push_buffer(
            GST_APP_SRC(appsrc_),
            buffer
        );

    if (result != GST_FLOW_OK &&
        measurement_enabled_)
    {
        std::lock_guard<std::mutex> lock(
            encoding_mutex_
        );

        pending_encodings_.erase(
            gst_util_uint64_scale(
                frame.metadata.frame_id - 1,
                GST_SECOND,
                config_.fps
            )
        );
    }


    return result == GST_FLOW_OK;
}


std::vector<StageMetric> Streaming::encoding_metrics() const
{
    std::lock_guard<std::mutex> lock(
        encoding_mutex_
    );

    return encoding_metrics_;
}


NetworkMetricSummary Streaming::network_metrics() const
{
    std::lock_guard<std::mutex> lock(
        encoding_mutex_
    );

    NetworkMetricSummary summary;
    summary.rtp_packet_count = rtp_packet_count_;
    summary.rtp_byte_count = rtp_byte_count_;
    summary.metadata_injected_count =
        metadata_injected_count_;
    summary.metadata_lookup_miss_count =
        metadata_lookup_miss_count_;
    summary.metadata_extension_failure_count =
        metadata_extension_failure_count_;

    if (first_rtp_packet_at_ !=
            std::chrono::steady_clock::time_point{} &&
        last_rtp_packet_at_ > first_rtp_packet_at_)
    {
        summary.duration_seconds =
            std::chrono::duration<double>(
                last_rtp_packet_at_ -
                first_rtp_packet_at_
            ).count();
        summary.average_bitrate_kbps =
            static_cast<double>(rtp_byte_count_) * 8.0 /
            summary.duration_seconds / 1000.0;
    }

    return summary;
}


GstPadProbeReturn Streaming::encoder_sink_probe(
    GstPad*,
    GstPadProbeInfo* info,
    gpointer user_data)
{
    auto* streaming =
        static_cast<Streaming*>(user_data);

    GstBuffer* buffer =
        GST_PAD_PROBE_INFO_BUFFER(info);

    if (buffer)
        streaming->record_encoder_input(buffer);

    return GST_PAD_PROBE_OK;
}


GstPadProbeReturn Streaming::encoder_src_probe(
    GstPad*,
    GstPadProbeInfo* info,
    gpointer user_data)
{
    auto* streaming =
        static_cast<Streaming*>(user_data);

    GstBuffer* buffer =
        GST_PAD_PROBE_INFO_BUFFER(info);

    if (buffer)
        streaming->record_encoder_output(buffer);

    return GST_PAD_PROBE_OK;
}


GstPadProbeReturn Streaming::payloader_src_probe(
    GstPad*,
    GstPadProbeInfo* info,
    gpointer user_data)
{
    auto* streaming =
        static_cast<Streaming*>(user_data);

    streaming->add_rtp_metadata(info);

    return GST_PAD_PROBE_OK;
}


void Streaming::record_encoder_input(GstBuffer* buffer)
{
    const GstClockTime pts =
        GST_BUFFER_PTS(buffer);

    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return;

    const auto entered_at =
        std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(
        encoding_mutex_
    );

    const auto pending =
        pending_encodings_.find(pts);

    if (pending == pending_encodings_.end())
        return;

    if (!GST_CLOCK_TIME_IS_VALID(
            first_encoder_input_pts_))
    {
        first_encoder_input_pts_ = pts;
    }

    pending->second.entered_at = entered_at;
}


void Streaming::record_encoder_output(GstBuffer* buffer)
{
    const GstClockTime pts =
        GST_BUFFER_PTS(buffer);

    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return;

    const auto exited_at =
        std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(
        encoding_mutex_
    );

    /*
     * x264enc는 DTS가 음수가 되는 것을 피하기 위해 입력 PTS 전체에
     * 고정 오프셋을 더할 수 있다. 최초 입력 Frame은 Stream의 첫
     * IDR Frame이므로 최초 유효 출력과 대응한다. 여기서 실제 오프셋을
     * 한 번 구하고 이후 출력 PTS를 입력 PTS 영역으로 정규화한다.
     * 따라서 B-frame 재정렬이 있어도 출력 순서가 아니라 정규화된
     * 원래 PTS를 키로 Frame을 찾는다.
     */
    if (!GST_CLOCK_TIME_IS_VALID(
            encoder_pts_offset_))
    {
        if (!GST_CLOCK_TIME_IS_VALID(
                first_encoder_input_pts_) ||
            pts < first_encoder_input_pts_)
        {
            return;
        }

        encoder_pts_offset_ =
            pts - first_encoder_input_pts_;
    }

    if (pts < encoder_pts_offset_)
        return;

    const GstClockTime input_pts =
        pts - encoder_pts_offset_;

    const auto pending =
        pending_encodings_.find(input_pts);

    if (pending == pending_encodings_.end())
        return;

    // Payloader 출력은 Encoder가 부여한 실제 출력 PTS를 유지한다.
    if (pending->second.captured_system_ns != 0)
    {
        encoded_frame_metadata_[pts] = {
            pending->second.frame_id,
            pending->second.captured_system_ns
        };
    }

    /*
     * 이 값은 순수 CPU 연산 시간이 아니라 Encoder sink 입력부터
     * 대응하는 Encoder src 출력까지의 buffering을 포함한 경과 시간이다.
     * 입력 probe를 통과하지 않았거나 PTS가 대응되지 않은 Frame은
     * 잘못된 latency를 만들지 않고 기록에서 제외한다.
     */
    if (pending->second.entered_at !=
            std::chrono::steady_clock::time_point{} &&
        encoding_metrics_.size() < kMaxStageMetrics)
    {
        StageMetric metric;
        metric.frame_id = pending->second.frame_id;
        metric.processing_ms =
            std::chrono::duration<double, std::milli>(
                exited_at - pending->second.entered_at
            ).count();

        encoding_metrics_.push_back(metric);
    }

    pending_encodings_.erase(pending);
}


void Streaming::add_rtp_metadata(GstPadProbeInfo* info)
{
    GstBuffer* buffer =
        GST_PAD_PROBE_INFO_BUFFER(info);

    if (!buffer)
        return;

    {
        std::lock_guard<std::mutex> lock(
            encoding_mutex_
        );
        const auto now = std::chrono::steady_clock::now();
        if (first_rtp_packet_at_ ==
            std::chrono::steady_clock::time_point{})
        {
            first_rtp_packet_at_ = now;
        }
        last_rtp_packet_at_ = now;
        ++rtp_packet_count_;
        rtp_byte_count_ += gst_buffer_get_size(buffer);
    }

    const GstClockTime pts =
        GST_BUFFER_PTS(buffer);

    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return;

    RtpFrameMetadata metadata;

    {
        std::lock_guard<std::mutex> lock(
            encoding_mutex_
        );

        const auto encoded =
            encoded_frame_metadata_.find(pts);

        if (encoded == encoded_frame_metadata_.end())
        {
            ++metadata_lookup_miss_count_;
            return;
        }

        metadata = encoded->second;
    }

    buffer = gst_buffer_make_writable(buffer);

    if (!buffer)
        return;

    GST_PAD_PROBE_INFO_DATA(info) = buffer;

    std::array<guint8, kE2eMetadataSize> extension{};

    const guint64 frame_id_be =
        GUINT64_TO_BE(metadata.frame_id);

    const guint64 captured_ns_be =
        GUINT64_TO_BE(
            static_cast<guint64>(
                metadata.captured_system_ns
            )
        );

    std::memcpy(
        extension.data(),
        &frame_id_be,
        sizeof(frame_id_be)
    );

    std::memcpy(
        extension.data() + sizeof(frame_id_be),
        &captured_ns_be,
        sizeof(captured_ns_be)
    );

    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;

    if (!gst_rtp_buffer_map(
            buffer,
            GST_MAP_READWRITE,
            &rtp))
    {
        return;
    }

    const gboolean marker =
        gst_rtp_buffer_get_marker(&rtp);

    const gboolean extension_added =
        gst_rtp_buffer_add_extension_onebyte_header(
        &rtp,
        kE2eMetadataExtensionId,
        extension.data(),
        extension.size()
    );

    if (extension_added)
        ++metadata_injected_count_;
    else
        ++metadata_extension_failure_count_;

    gst_rtp_buffer_unmap(&rtp);

    if (marker)
    {
        std::lock_guard<std::mutex> lock(
            encoding_mutex_
        );

        encoded_frame_metadata_.erase(pts);
    }
}


bool Streaming::install_encoding_probes()
{
    encoder_sink_pad_ =
        gst_element_get_static_pad(
            encoder_,
            "sink"
        );

    encoder_src_pad_ =
        gst_element_get_static_pad(
            encoder_,
            "src"
        );

    payloader_src_pad_ =
        gst_element_get_static_pad(
            payloader_,
            "src"
        );

    if (!encoder_sink_pad_ ||
        !encoder_src_pad_ ||
        !payloader_src_pad_)
    {
        remove_encoding_probes();
        return false;
    }

    encoder_sink_probe_id_ =
        gst_pad_add_probe(
            encoder_sink_pad_,
            GST_PAD_PROBE_TYPE_BUFFER,
            &Streaming::encoder_sink_probe,
            this,
            nullptr
        );

    encoder_src_probe_id_ =
        gst_pad_add_probe(
            encoder_src_pad_,
            GST_PAD_PROBE_TYPE_BUFFER,
            &Streaming::encoder_src_probe,
            this,
            nullptr
        );

    payloader_src_probe_id_ =
        gst_pad_add_probe(
            payloader_src_pad_,
            GST_PAD_PROBE_TYPE_BUFFER,
            &Streaming::payloader_src_probe,
            this,
            nullptr
        );

    if (encoder_sink_probe_id_ == 0 ||
        encoder_src_probe_id_ == 0 ||
        payloader_src_probe_id_ == 0)
    {
        remove_encoding_probes();
        return false;
    }

    return true;
}


void Streaming::remove_encoding_probes()
{
    if (encoder_sink_pad_ &&
        encoder_sink_probe_id_ != 0)
    {
        gst_pad_remove_probe(
            encoder_sink_pad_,
            encoder_sink_probe_id_
        );
    }

    if (encoder_src_pad_ &&
        encoder_src_probe_id_ != 0)
    {
        gst_pad_remove_probe(
            encoder_src_pad_,
            encoder_src_probe_id_
        );
    }

    if (payloader_src_pad_ &&
        payloader_src_probe_id_ != 0)
    {
        gst_pad_remove_probe(
            payloader_src_pad_,
            payloader_src_probe_id_
        );
    }

    encoder_sink_probe_id_ = 0;
    encoder_src_probe_id_ = 0;
    payloader_src_probe_id_ = 0;

    if (encoder_sink_pad_)
    {
        gst_object_unref(encoder_sink_pad_);
        encoder_sink_pad_ = nullptr;
    }

    if (encoder_src_pad_)
    {
        gst_object_unref(encoder_src_pad_);
        encoder_src_pad_ = nullptr;
    }

    if (payloader_src_pad_)
    {
        gst_object_unref(payloader_src_pad_);
        payloader_src_pad_ = nullptr;
    }
}


void Streaming::close()
{
    if (!pipeline_)
        return;

    if (appsrc_)
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));

    if (measurement_enabled_ && opened_ && appsrc_)
    {
        // Encoder 내부에 남은 Frame의 출력 probe가 실행될 시간을 준다.
        GstBus* bus =
            gst_element_get_bus(pipeline_);

        if (bus)
        {
            GstMessage* message =
                gst_bus_timed_pop_filtered(
                    bus,
                    5 * GST_SECOND,
                    static_cast<GstMessageType>(
                        GST_MESSAGE_EOS |
                        GST_MESSAGE_ERROR
                    )
                );

            if (message)
                gst_message_unref(message);
            else
                Logger::warn(
                    "[Streaming] EOS 대기 시간 초과"
                );

            gst_object_unref(bus);
        }
    }

    gst_element_set_state(
        pipeline_,
        GST_STATE_NULL
    );

    // NULL 전환 후 probe와 pad 참조를 해제하여 callback 수명을 끝낸다.
    remove_encoding_probes();

    {
        std::lock_guard<std::mutex> lock(
            encoding_mutex_
        );

        // 출력 PTS와 대응되지 않은 Frame은 측정값으로 남기지 않는다.
        pending_encodings_.clear();
        encoded_frame_metadata_.clear();
        first_encoder_input_pts_ = GST_CLOCK_TIME_NONE;
        encoder_pts_offset_ = GST_CLOCK_TIME_NONE;
    }

    gst_object_unref(pipeline_);

    pipeline_ = nullptr;
    appsrc_ = nullptr;
    queue_ = nullptr;
    converter_ = nullptr;
    encoder_capsfilter_ = nullptr;
    encoder_ = nullptr;
    parser_ = nullptr;
    payloader_ = nullptr;
    sink_ = nullptr;

    opened_ = false;

    Logger::info("[Streaming] 종료");
}
