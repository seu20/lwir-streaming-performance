#include "common/metrics_report.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>


void save_stage_metrics_csv(
    const std::string& filename,
    const std::vector<StageMetric>& metrics)
{
    std::ofstream file(filename);

    if (!file.is_open())
    {
        std::cerr
            << "[Metrics] CSV 열기 실패: "
            << filename << '\n';

        return;
    }

    file
        << "frame_id,"
        << "queue_wait_ms,"
        << "processing_ms,"
        << "e2e_ms\n";

    file << std::fixed << std::setprecision(3);

    for (const auto& metric : metrics)
    {
        file << metric.frame_id << ",";

        if (metric.queue_wait_ms)
            file << *metric.queue_wait_ms;

        file << ",";

        file << metric.processing_ms << ",";

        if (metric.e2e_ms)
            file << *metric.e2e_ms;

        file << "\n";
    }
}


void save_encoding_metrics_csv(
    const std::string& filename,
    const std::vector<StageMetric>& metrics)
{
    std::ofstream file(filename);

    if (!file.is_open())
    {
        std::cerr
            << "[Metrics] CSV 열기 실패: "
            << filename << '\n';

        return;
    }

    file << "frame_id,encoding_ms\n";
    file << std::fixed << std::setprecision(3);

    for (const auto& metric : metrics)
    {
        file
            << metric.frame_id << ","
            << metric.processing_ms << "\n";
    }
}


void save_fps_metrics_csv(
    const std::string& filename,
    const std::vector<FpsMetric>& capture_metrics,
    const std::vector<FpsMetric>& streaming_metrics)
{
    std::ofstream file(filename);

    if (!file.is_open())
    {
        std::cerr
            << "[Metrics] CSV 열기 실패: "
            << filename << '\n';
        return;
    }

    file
        << "stage,interval_index,interval_start_s,"
        << "interval_end_s,frame_count,fps\n";
    file << std::fixed << std::setprecision(3);

    const auto write_metrics =
        [&file](
            const std::string& stage,
            const std::vector<FpsMetric>& metrics)
        {
            for (std::size_t index = 0;
                 index < metrics.size();
                 ++index)
            {
                const auto& metric = metrics[index];

                file
                    << stage << ","
                    << index + 1 << ","
                    << metric.interval_start_s << ","
                    << metric.interval_end_s << ","
                    << metric.frame_count << ","
                    << metric.fps << "\n";
            }
        };

    write_metrics("capture", capture_metrics);
    write_metrics("streaming_submit", streaming_metrics);
}


void save_frame_metrics_csv(
    const std::string& filename,
    const FrameMetricSummary& metrics)
{
    std::ofstream file(filename);

    if (!file.is_open())
    {
        std::cerr
            << "[Metrics] CSV 열기 실패: "
            << filename << '\n';
        return;
    }

    const std::uint64_t preprocess_attempt_count =
        metrics.preprocess_success_count +
        metrics.preprocess_failure_count;

    const std::uint64_t streaming_attempt_count =
        metrics.streaming_submit_success_count +
        metrics.streaming_submit_failure_count;

    const std::uint64_t queue_push_attempt_count =
        metrics.capture_success_count +
        metrics.preprocess_success_count;

    const auto rate = [](
        std::uint64_t count,
        std::uint64_t denominator)
    {
        if (denominator == 0)
            return 0.0;

        return 100.0 *
            static_cast<double>(count) /
            static_cast<double>(denominator);
    };

    file
        << "capture_success_count,"
        << "preprocess_attempt_count,"
        << "preprocess_success_count,"
        << "preprocess_failure_count,"
        << "preprocess_failure_rate_pct,"
        << "streaming_submit_attempt_count,"
        << "streaming_submit_success_count,"
        << "streaming_submit_failure_count,"
        << "streaming_submit_failure_rate_pct,"
        << "queue_push_attempt_count,"
        << "queue_policy_drop_count,"
        << "queue_policy_drop_rate_pct,"
        << "capture_queue_push_failure_count,"
        << "streaming_queue_push_failure_count,"
        << "dataset_deadline_miss_count\n";

    file << std::fixed << std::setprecision(3);
    file
        << metrics.capture_success_count << ","
        << preprocess_attempt_count << ","
        << metrics.preprocess_success_count << ","
        << metrics.preprocess_failure_count << ","
        << rate(
               metrics.preprocess_failure_count,
               preprocess_attempt_count
           ) << ","
        << streaming_attempt_count << ","
        << metrics.streaming_submit_success_count << ","
        << metrics.streaming_submit_failure_count << ","
        << rate(
               metrics.streaming_submit_failure_count,
               streaming_attempt_count
           ) << ","
        << queue_push_attempt_count << ","
        << metrics.queue_policy_drop_count << ","
        << rate(
               metrics.queue_policy_drop_count,
               queue_push_attempt_count
           ) << ","
        << metrics.capture_queue_push_failure_count << ","
        << metrics.streaming_queue_push_failure_count << ","
        << metrics.dataset_deadline_miss_count << "\n";
}


void print_stage_summary(
    const std::string& stage_name,
    const std::vector<StageMetric>& metrics)
{
    if (metrics.empty())
    {
        std::cout
            << "[Metrics] "
            << stage_name
            << ": 측정값 없음\n";

        return;
    }

    std::vector<double> processing_values;

    processing_values.reserve(metrics.size());

    for (const auto& metric : metrics)
        processing_values.push_back(metric.processing_ms);

    std::sort(
        processing_values.begin(),
        processing_values.end()
    );

    const double sum =
        std::accumulate(
            processing_values.begin(),
            processing_values.end(),
            0.0
        );

    const double average =
        sum / processing_values.size();

    const double maximum =
        processing_values.back();

    const std::size_t p95_index =
        static_cast<std::size_t>(
            0.95 * (processing_values.size() - 1)
        );

    const std::size_t p99_index =
        static_cast<std::size_t>(
            0.99 * (processing_values.size() - 1)
        );

    std::cout
        << std::fixed
        << std::setprecision(3)

        << "[Metrics] "
        << stage_name

        << " count="
        << processing_values.size()

        << " avg="
        << average
        << " ms"

        << " max="
        << maximum
        << " ms"

        << " p95="
        << processing_values[p95_index]
        << " ms"

        << " p99="
        << processing_values[p99_index]
        << " ms\n";
}


void print_fps_summary(
    const std::string& stage_name,
    std::uint64_t frame_count,
    double duration_seconds)
{
    const double average_fps =
        duration_seconds > 0.0
            ? static_cast<double>(frame_count) /
                duration_seconds
            : 0.0;

    std::cout
        << std::fixed
        << std::setprecision(3)
        << "[Metrics] " << stage_name
        << " frames=" << frame_count
        << " duration=" << duration_seconds << " s"
        << " average=" << average_fps << " FPS\n";
}


void print_frame_summary(
    const FrameMetricSummary& metrics)
{
    std::cout
        << "[Metrics] Frame counts"
        << " capture=" << metrics.capture_success_count
        << " preprocess=" << metrics.preprocess_success_count
        << " streaming_submit="
        << metrics.streaming_submit_success_count
        << " queue_policy_drop="
        << metrics.queue_policy_drop_count
        << " preprocess_failure="
        << metrics.preprocess_failure_count
        << " streaming_submit_failure="
        << metrics.streaming_submit_failure_count
        << " dataset_deadline_miss="
        << metrics.dataset_deadline_miss_count
        << '\n';
}
