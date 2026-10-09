#include "pipeline/capture_thread.hpp"

#include <chrono>
#include <thread>
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


// 선택한 입력 소스에서 Frame을 읽어 다음 Stage Queue로 전달한다.
void CaptureThread::run()
{
    Logger::info("[CaptureThread] 시작");

    if (!capture_.open())
    {
        Logger::error("[CaptureThread] 입력 소스를 열지 못했습니다");
        output_queue_.close();
        return;
    }

    const bool dataset_mode =
        capture_.is_dataset_mode();

    auto dataset_frame_interval =
        std::chrono::steady_clock::duration::zero();

    if (dataset_mode)
    {
        dataset_frame_interval =
            std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(
                        1.0 / capture_.input_fps()
                    )
                );
    }

    std::chrono::steady_clock::time_point
        dataset_schedule_started_at{};
    auto dataset_schedule_delay =
        std::chrono::steady_clock::duration::zero();
    std::uint64_t dataset_frame_index = 0;

    std::uint64_t next_frame_id = 1;

    const auto fps_started_at =
        std::chrono::steady_clock::now();

    auto fps_interval_started_at =
        fps_started_at;

    std::uint64_t fps_interval_frame_count = 0;

    while (true)
    {
        Frame frame;

        const auto started_at =
            std::chrono::steady_clock::now();

        if (!capture_.capture(frame))
            break;

        const auto read_finished_at =
            std::chrono::steady_clock::now();

        auto supplied_at = read_finished_at;

        if (dataset_mode)
        {
            if (dataset_frame_index == 0)
            {
                // 첫 파일을 읽은 직후를 Dataset 공급 스케줄의 0 ms로 둔다.
                // 따라서 첫 디스크 읽기 시간은 pacing 지터에 포함되지 않는다.
                dataset_schedule_started_at = read_finished_at;
            }
            else
            {
                auto supply_target =
                    dataset_schedule_started_at +
                    dataset_frame_interval *
                        static_cast<std::int64_t>(
                            dataset_frame_index
                        ) +
                    dataset_schedule_delay;

                // 디스크 읽기가 목표 시각을 넘긴 경우에는 지연량만큼
                // 이후 전체 스케줄을 이동한다. 원래 스케줄을 따라잡기
                // 위한 연속 Queue push는 하지 않는다.
                bool deadline_missed = false;

                if (read_finished_at > supply_target)
                {
                    dataset_schedule_delay +=
                        read_finished_at - supply_target;
                    supply_target = read_finished_at;
                    deadline_missed = true;
                }

                if (read_finished_at < supply_target)
                    std::this_thread::sleep_until(supply_target);

                supplied_at =
                    std::chrono::steady_clock::now();

                // Thread scheduling이 한 Frame 간격 이상 늦어진 경우에도
                // 이후 목표 시각을 이동하여 burst를 방지한다.
                if (supplied_at - supply_target >=
                    dataset_frame_interval)
                {
                    dataset_schedule_delay +=
                        supplied_at - supply_target;
                    deadline_missed = true;
                }

                if (measurement_enabled_ &&
                    deadline_missed)
                {
                    ++dataset_deadline_miss_count_;
                }
            }

            ++dataset_frame_index;
        }

        std::chrono::system_clock::time_point
            captured_system_at{};

        if (measurement_enabled_)
        {
            captured_system_at =
                std::chrono::system_clock::now();
        }

        // Camera는 수신 완료, Dataset은 실제 Queue 공급 직전 시각을
        // E2E 시작 기준으로 사용한다.
        frame.metadata.frame_id = next_frame_id++;
        frame.metadata.captured_at = supplied_at;
        frame.metadata.captured_system_at =
            captured_system_at;

        if (measurement_enabled_)
        {
            ++success_count_;

            if (!dataset_mode)
            {
                record_fps_frame(
                    fps_metrics_,
                    fps_started_at,
                    fps_interval_started_at,
                    fps_interval_frame_count,
                    supplied_at
                );
            }

            // Capture 앞에는 Application Queue가 없으므로
            // enqueued_at은 빈 time_point를 전달한다.
            // Dataset에서는 의도적인 FPS 대기시간을 제외하고
            // PNG 파일 읽기 시간만 processing_ms에 기록한다.
            record_stage_metric(
                metrics_,
                frame.metadata,
                {},
                started_at,
                read_finished_at
            );
        }

        // 다음 Stage의 Queue Wait 측정을 위한 기준 시각.
        frame.metadata.enqueued_at = supplied_at;

        if (!output_queue_.push(std::move(frame)))
        {
            if (measurement_enabled_)
                ++queue_push_failure_count_;

            break;
        }

        // Dataset FPS는 파일 읽기 완료가 아니라 실제 Queue 공급 시각을
        // 기준으로 기록한다.
        if (measurement_enabled_ && dataset_mode)
        {
            record_fps_frame(
                fps_metrics_,
                fps_started_at,
                fps_interval_started_at,
                fps_interval_frame_count,
                supplied_at
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

    capture_.close();

    // 다음 Stage에 더 이상 Frame이 들어오지 않음을 알린다.
    output_queue_.close();

    Logger::info("[CaptureThread] 종료");
}
