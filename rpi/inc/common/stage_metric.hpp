#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <optional>
#include <vector>

#include "common/metadata.hpp"

// 프레임별 큐 대기, 처리, 필요한 경우 전체 지연을 기록한다.
struct StageMetric
{
    std::uint64_t frame_id = 0;
    std::optional<double> queue_wait_ms;
    double processing_ms = 0.0;
    std::optional<double> e2e_ms;
};

// 고정 1초 구간 또는 종료 시 남은 부분 구간의 FPS 측정값.
struct FpsMetric
{
    double interval_start_s = 0.0;
    double interval_end_s = 0.0;
    std::uint64_t frame_count = 0;
    double fps = 0.0;
};

// Raspberry Pi 내부에서 직접 관측 가능한 Frame 처리 결과.
struct FrameMetricSummary
{
    std::uint64_t capture_success_count = 0;
    std::uint64_t preprocess_success_count = 0;
    std::uint64_t streaming_submit_success_count = 0;
    std::uint64_t queue_policy_drop_count = 0;
    std::uint64_t preprocess_failure_count = 0;
    std::uint64_t streaming_submit_failure_count = 0;
    std::uint64_t capture_queue_push_failure_count = 0;
    std::uint64_t streaming_queue_push_failure_count = 0;
    std::uint64_t dataset_deadline_miss_count = 0;
};

// 장시간 실행 시 측정 기록의 메모리 사용량을 제한한다.
constexpr std::size_t kMaxStageMetrics = 10000;
constexpr std::size_t kMaxFpsMetrics = 10000;

inline void append_fps_metric(
    std::vector<FpsMetric>& metrics,
    std::chrono::steady_clock::time_point measurement_started_at,
    std::chrono::steady_clock::time_point interval_started_at,
    std::chrono::steady_clock::time_point interval_finished_at,
    std::uint64_t frame_count)
{
    const double interval_seconds =
        std::chrono::duration<double>(
            interval_finished_at - interval_started_at
        ).count();

    if (interval_seconds <= 0.0 ||
        metrics.size() >= kMaxFpsMetrics)
    {
        return;
    }

    FpsMetric metric;
    metric.interval_start_s =
        std::chrono::duration<double>(
            interval_started_at - measurement_started_at
        ).count();
    metric.interval_end_s =
        std::chrono::duration<double>(
            interval_finished_at - measurement_started_at
        ).count();
    metric.frame_count = frame_count;
    metric.fps =
        static_cast<double>(frame_count) /
        interval_seconds;

    metrics.push_back(metric);
}

inline void close_completed_fps_intervals(
    std::vector<FpsMetric>& metrics,
    std::chrono::steady_clock::time_point measurement_started_at,
    std::chrono::steady_clock::time_point& interval_started_at,
    std::uint64_t& interval_frame_count,
    std::chrono::steady_clock::time_point now)
{
    constexpr auto interval =
        std::chrono::seconds(1);

    while (now - interval_started_at >= interval)
    {
        const auto interval_finished_at =
            interval_started_at + interval;

        append_fps_metric(
            metrics,
            measurement_started_at,
            interval_started_at,
            interval_finished_at,
            interval_frame_count
        );

        interval_started_at = interval_finished_at;
        interval_frame_count = 0;
    }
}

inline void record_fps_frame(
    std::vector<FpsMetric>& metrics,
    std::chrono::steady_clock::time_point measurement_started_at,
    std::chrono::steady_clock::time_point& interval_started_at,
    std::uint64_t& interval_frame_count,
    std::chrono::steady_clock::time_point frame_at)
{
    close_completed_fps_intervals(
        metrics,
        measurement_started_at,
        interval_started_at,
        interval_frame_count,
        frame_at
    );

    ++interval_frame_count;
}

inline void finish_fps_measurement(
    std::vector<FpsMetric>& metrics,
    std::chrono::steady_clock::time_point measurement_started_at,
    std::chrono::steady_clock::time_point& interval_started_at,
    std::uint64_t& interval_frame_count,
    std::chrono::steady_clock::time_point finished_at)
{
    close_completed_fps_intervals(
        metrics,
        measurement_started_at,
        interval_started_at,
        interval_frame_count,
        finished_at
    );

    append_fps_metric(
        metrics,
        measurement_started_at,
        interval_started_at,
        finished_at,
        interval_frame_count
    );

    interval_frame_count = 0;
}

inline void record_stage_metric(
    std::vector<StageMetric>& metrics,
    const Metadata& metadata,
    std::chrono::steady_clock::time_point enqueued_at,
    std::chrono::steady_clock::time_point started_at,
    std::chrono::steady_clock::time_point finished_at,
    bool include_e2e = false)
{
    if (metrics.size() >= kMaxStageMetrics)
        return;

    StageMetric metric;
    metric.frame_id = metadata.frame_id;
    metric.processing_ms = std::chrono::duration<double, std::milli>(finished_at - started_at).count();

    if (enqueued_at != std::chrono::steady_clock::time_point{}){
        metric.queue_wait_ms = std::chrono::duration<double, std::milli>(started_at - enqueued_at).count();
    }

    if (include_e2e && metadata.captured_at != std::chrono::steady_clock::time_point{})
    {
        metric.e2e_ms = std::chrono::duration<double, std::milli>(finished_at - metadata.captured_at).count();
    }
    
    metrics.push_back(metric);
}
