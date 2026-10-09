#pragma once

#include <pthread.h>
#include <vector>

#include "app/capture.hpp"
#include "common/frame.hpp"
#include "common/stage_metric.hpp"
#include "common/threadsafequeue.hpp"


class CaptureThread
{
public:
    CaptureThread(
        Capture& capture,
        ThreadSafeQueue<Frame>& output_queue,
        bool measurement_enabled);

    void start();
    void join();

    const std::vector<StageMetric>& metrics() const
    {
        return metrics_;
    }

    const std::vector<FpsMetric>& fps_metrics() const
    {
        return fps_metrics_;
    }

    std::uint64_t success_count() const
    {
        return success_count_;
    }

    std::uint64_t queue_push_failure_count() const
    {
        return queue_push_failure_count_;
    }

    double fps_duration_seconds() const
    {
        return fps_duration_seconds_;
    }

    std::uint64_t dataset_deadline_miss_count() const
    {
        return dataset_deadline_miss_count_;
    }
private:
    static void* thread_func(void* arg);
    void run();

    Capture& capture_;
    ThreadSafeQueue<Frame>& output_queue_;

    bool measurement_enabled_;
    pthread_t thread_{};

    std::vector<StageMetric> metrics_;
    std::vector<FpsMetric> fps_metrics_;
    std::uint64_t success_count_ = 0;
    std::uint64_t queue_push_failure_count_ = 0;
    std::uint64_t dataset_deadline_miss_count_ = 0;
    double fps_duration_seconds_ = 0.0;
};
