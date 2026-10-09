#pragma once

#include <string>
#include <vector>

#include "common/stage_metric.hpp"


void save_stage_metrics_csv(
    const std::string& filename,
    const std::vector<StageMetric>& metrics);

void save_encoding_metrics_csv(
    const std::string& filename,
    const std::vector<StageMetric>& metrics);

void save_fps_metrics_csv(
    const std::string& filename,
    const std::vector<FpsMetric>& capture_metrics,
    const std::vector<FpsMetric>& streaming_metrics);

void save_frame_metrics_csv(
    const std::string& filename,
    const FrameMetricSummary& metrics);

void print_stage_summary(
    const std::string& stage_name,
    const std::vector<StageMetric>& metrics);

void print_fps_summary(
    const std::string& stage_name,
    std::uint64_t frame_count,
    double duration_seconds);

void print_frame_summary(
    const FrameMetricSummary& metrics);
