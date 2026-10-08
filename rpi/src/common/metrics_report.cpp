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