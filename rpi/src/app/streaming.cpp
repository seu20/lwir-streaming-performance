#include "app/streaming.hpp"

#include <cstring>
#include <utility>

#include "common/logger.hpp"


Streaming::Streaming(StreamingConfig config)
    : config_(std::move(config))
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
    encoder_   = gst_element_factory_make("x264enc", "encoder");
    parser_    = gst_element_factory_make("h264parse", "parser");
    payloader_ = gst_element_factory_make("rtph264pay", "payloader");
    sink_      = gst_element_factory_make("udpsink", "sink");

    if (!pipeline_ || !appsrc_ || !queue_ ||
        !encoder_ || !parser_ || !payloader_ || !sink_)
    {
        Logger::error("[Streaming] GStreamer element 생성 실패");
        return false;
    }

    // Preprocess 출력 형식:
    // 640x480 / GRAY8 / 30 FPS
    //
    // x264enc가 GRAY8 입력을 직접 받을 수 있으므로
    // baseline에서는 videoconvert를 사용하지 않는다.
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
    // 지금은 위 옵션을 넣지 않는다.


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
    //   → x264enc       : H.264 Encoding
    //   → h264parse     : H.264 bitstream 정리
    //   → rtph264pay    : RTP packet 생성
    //   → udpsink       : UDP 전송
    gst_bin_add_many(
        GST_BIN(pipeline_),
        appsrc_,
        queue_,
        encoder_,
        parser_,
        payloader_,
        sink_,
        nullptr
    );

    if (!gst_element_link_many(
            appsrc_,
            queue_,
            encoder_,
            parser_,
            payloader_,
            sink_,
            nullptr))
    {
        Logger::error("[Streaming] Pipeline link 실패");
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
    GstBuffer* buffer =
        gst_buffer_new_allocate(
            nullptr,
            buffer_size,
            nullptr
        );

    if (!buffer)
        return false;


    GstMapInfo map{};

    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE))
    {
        gst_buffer_unref(buffer);
        return false;
    }


    // OpenCV Frame → GstBuffer 복사.
    //
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


    // Frame 하나의 재생 시간.
    // 30 FPS라면 약 33.3 ms.
    GST_BUFFER_DURATION(buffer) =
        gst_util_uint64_scale_int(
            1,
            GST_SECOND,
            config_.fps
        );


    // GstBuffer를 GStreamer 내부 Pipeline으로 전달.
    //
    // 여기서 반환되었다고 H.264 Encoding이 끝난 것은 아니다.
    // appsrc 내부 Queue에 Frame을 전달한 것뿐이다.
    const GstFlowReturn result =
        gst_app_src_push_buffer(
            GST_APP_SRC(appsrc_),
            buffer
        );


    return result == GST_FLOW_OK;
}


void Streaming::close()
{
    if (!pipeline_)
        return;

    if (appsrc_)
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));

    gst_element_set_state(
        pipeline_,
        GST_STATE_NULL
    );

    gst_object_unref(pipeline_);

    pipeline_ = nullptr;
    appsrc_ = nullptr;
    queue_ = nullptr;
    encoder_ = nullptr;
    parser_ = nullptr;
    payloader_ = nullptr;
    sink_ = nullptr;

    opened_ = false;

    Logger::info("[Streaming] 종료");
}