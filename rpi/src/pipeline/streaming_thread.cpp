#include "pipeline/streaming_thread.hpp"

#include <chrono>

#include "common/logger.hpp"


StreamingThread::StreamingThread(
    Streaming& streaming,
    ThreadSafeQueue<Frame>& input_queue,
    bool measurement_enabled)
    : streaming_(streaming),
      input_queue_(input_queue),
      measurement_enabled_(measurement_enabled)
{
}


// Streaming pthread를 시작한다.
void StreamingThread::start()
{
    pthread_create(
        &thread_,
        nullptr,
        &StreamingThread::thread_func,
        this
    );
}


// Streaming pthread가 끝날 때까지 기다린다.
void StreamingThread::join()
{
    pthread_join(
        thread_,
        nullptr
    );
}


// pthread에서 StreamingThread::run()을 실행한다.
void* StreamingThread::thread_func(void* arg)
{
    auto* thread =
        static_cast<StreamingThread*>(arg);

    thread->run();

    return nullptr;
}


// Preprocess 결과 Frame을 받아 GStreamer에 전달한다.
void StreamingThread::run()
{
    Logger::info("[StreamingThread] 시작");


    if (!streaming_.open())
    {
        Logger::error(
            "[StreamingThread] Streaming Pipeline을 열지 못했습니다"
        );

        return;
    }


    const auto fps_started_at =
        std::chrono::steady_clock::now();

    auto fps_interval_started_at =
        fps_started_at;

    std::uint64_t fps_interval_frame_count = 0;


    Frame frame;


    while (input_queue_.pop(frame))
    {
        const auto started_at =
            std::chrono::steady_clock::now();


        if (!streaming_.push(frame))
        {
            if (measurement_enabled_)
                ++failure_count_;

            Logger::error(
                "[StreamingThread] Frame 전송 실패"
            );

            break;
        }


        const auto finished_at =
            std::chrono::steady_clock::now();


        if (measurement_enabled_)
        {
            ++success_count_;

            record_fps_frame(
                fps_metrics_,
                fps_started_at,
                fps_interval_started_at,
                fps_interval_frame_count,
                finished_at
            );

            /*
             * 여기서 측정되는 것:
             *
             * Queue Wait
             * +
             * GstBuffer Copy / appsrc submission
             *
             * 실제 Encoder / Network 완료시간은 아님.
             */
            record_stage_metric(
                submission_metrics_,
                frame.metadata,
                frame.metadata.enqueued_at,
                started_at,
                finished_at,
                false
            );
        }
    }


    if (measurement_enabled_)
    {
        const auto fps_finished_at =
            std::chrono::steady_clock::now();

        finish_fps_measurement(
            fps_metrics_,
            fps_started_at,
            fps_interval_started_at,
            fps_interval_frame_count,
            fps_finished_at
        );

        fps_duration_seconds_ =
            std::chrono::duration<double>(
                fps_finished_at - fps_started_at
            ).count();
    }


    streaming_.close();


    Logger::info("[StreamingThread] 종료");
}
