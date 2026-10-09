#include "pipeline/preprocess_thread.hpp"

#include <chrono>
#include <utility>

#include "common/logger.hpp"


PreprocessThread::PreprocessThread(
    Preprocess& preprocess,
    ThreadSafeQueue<Frame>& input_queue,
    ThreadSafeQueue<Frame>& output_queue,
    bool measurement_enabled)
    : preprocess_(preprocess),
      input_queue_(input_queue),
      output_queue_(output_queue),
      measurement_enabled_(measurement_enabled)
{
}


// Preprocess pthread를 시작한다.
void PreprocessThread::start()
{
    pthread_create(
        &thread_,
        nullptr,
        &PreprocessThread::thread_func,
        this
    );
}


// Preprocess pthread가 끝날 때까지 기다린다.
void PreprocessThread::join()
{
    pthread_join(thread_, nullptr);
}


// pthread에서 PreprocessThread::run()을 실행한다.
void* PreprocessThread::thread_func(void* arg)
{
    auto* thread =
        static_cast<PreprocessThread*>(arg);

    thread->run();

    return nullptr;
}


// Capture Queue에서 Frame을 받아 전처리 후 다음 Stage로 전달한다.
void PreprocessThread::run()
{
    Logger::info("[PreprocessThread] 시작");

    Frame frame;

    while (input_queue_.pop(frame))
    {
        const auto started_at =
            std::chrono::steady_clock::now();

        if (!preprocess_.process(frame))
        {
            if (measurement_enabled_)
                ++failure_count_;

            Logger::error("[PreprocessThread] Frame 전처리 실패");
            continue;
        }

        const auto finished_at =
            std::chrono::steady_clock::now();

        if (measurement_enabled_)
        {
            ++success_count_;

            record_stage_metric(
                metrics_,
                frame.metadata,
                frame.metadata.enqueued_at,
                started_at,
                finished_at
            );
        }

        // 이전 Queue의 enqueued_at은 이제 필요 없다.
        // 다음 Stage Queue에 들어가는 시각으로 덮어쓴다.
        frame.metadata.enqueued_at =
            std::chrono::steady_clock::now();

        if (!output_queue_.push(std::move(frame)))
        {
            if (measurement_enabled_)
                ++queue_push_failure_count_;

            break;
        }
    }

    // Encoding / Streaming Stage에
    // 더 이상 Frame이 들어오지 않음을 알린다.
    output_queue_.close();

    Logger::info("[PreprocessThread] 종료");
}
