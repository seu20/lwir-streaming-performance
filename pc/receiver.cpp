#include "receiver.hpp"

#include <iostream>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>


Receiver::Receiver(int port)
    : port_(port)
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
    const std::string pipeline_description =
        "udpsrc port=" + std::to_string(port_) +
        " caps=\"application/x-rtp,"
        "media=video,"
        "encoding-name=H264,"
        "payload=96,"
        "clock-rate=90000\" "
        "! rtph264depay "
        "! h264parse "
        "! avdec_h264 "
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

    if (!appsink_)
    {
        std::cerr
            << "[Receiver] appsink를 찾을 수 없습니다\n";

        close();
        return false;
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

            continue;
        }


        GstCaps* caps =
            gst_sample_get_caps(sample);

        GstBuffer* buffer =
            gst_sample_get_buffer(sample);

        if (!caps || !buffer)
        {
            gst_sample_unref(sample);
            continue;
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


    cv::destroyAllWindows();
}


void Receiver::close()
{
    if (pipeline_)
    {
        gst_element_set_state(
            pipeline_,
            GST_STATE_NULL
        );
    }

    if (appsink_)
    {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
    }

    if (pipeline_)
    {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }

    opened_ = false;
}