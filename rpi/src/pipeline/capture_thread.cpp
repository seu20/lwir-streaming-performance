#include "pipeline/capture_thread.hpp"

#include <chrono>
#include <utility>

#include "common/logger.hpp"


CaptureThread::CaptureThread(
    Capture& capture,
    ThreadSafeQueue<Frame>& output_queue,
    bool measurement_enabled)
    : capture_(capture),
      output_queue_(output_queue),
      measurement_enabled_(measurement_enabled)
{
}


// Capture pthread를 시작한다.
void CaptureThread::start()
{
    pthread_create(
        &thread_,
        nullptr,
        &CaptureThread::thread_func,
        this
    );
}


// Capture pthread가 끝날 때까지 기다린다.
void CaptureThread::join()
{
    pthread_join(thread_, nullptr);
}


// pthread에서 CaptureThread::run()을 실행한다.
void* CaptureThread::thread_func(void* arg)
{
    auto* thread =
        static_cast<CaptureThread*>(arg);

    thread->run();

    return nullptr;
}


// Camera에서 Frame을 읽어 다음 Stage Queue로 전달한다.
void CaptureThread::run()
{
    Logger::info("[CaptureThread] 시작");

    if (!capture_.open())
    {
        Logger::error("[CaptureThread] 카메라를 열지 못했습니다");
        output_queue_.close();
        return;
    }

    std::uint64_t next_frame_id = 1;

    while (true)
    {
        Frame frame;

        const auto started_at =
            std::chrono::steady_clock::now();

        if (!capture_.capture(frame))
            break;

        const auto finished_at =
            std::chrono::steady_clock::now();

        // Capture가 완료된 Frame에 식별자와 기준 시각을 부여한다.
        frame.metadata.frame_id = next_frame_id++;
        frame.metadata.captured_at = finished_at;

        if (measurement_enabled_)
        {
            // Capture 앞에는 Application Queue가 없으므로
            // enqueued_at은 빈 time_point를 전달한다.
            record_stage_metric(
                metrics_,
                frame.metadata,
                {},
                started_at,
                finished_at
            );
        }

        // 다음 Stage의 Queue Wait 측정을 위한 기준 시각.
        frame.metadata.enqueued_at =
            std::chrono::steady_clock::now();

        if (!output_queue_.push(std::move(frame)))
            break;
    }

    capture_.close();

    // 다음 Stage에 더 이상 Frame이 들어오지 않음을 알린다.
    output_queue_.close();

    Logger::info("[CaptureThread] 종료");
}