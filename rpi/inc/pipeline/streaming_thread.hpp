#pragma once

#include <pthread.h>
#include <vector>

#include "app/streaming.hpp"
#include "common/frame.hpp"
#include "common/stage_metric.hpp"
#include "common/threadsafequeue.hpp"


class StreamingThread
{
public:
    StreamingThread(
        Streaming& streaming,
        ThreadSafeQueue<Frame>& input_queue,
        bool measurement_enabled);

    void start();
    void join();
    const std::vector<StageMetric>& metrics() const
    {
        return submission_metrics_;
    }

    const std::vector<FpsMetric>& fps_metrics() const
    {
        return fps_metrics_;
    }

    std::uint64_t success_count() const
    {
        return success_count_;
    }

    std::uint64_t failure_count() const
    {
        return failure_count_;
    }

    double fps_duration_seconds() const
    {
        return fps_duration_seconds_;
    }

private:
    static void* thread_func(void* arg);
    void run();

    Streaming& streaming_;
    ThreadSafeQueue<Frame>& input_queue_;

    bool measurement_enabled_;

    pthread_t thread_{};

    /*
     * 주의:
     * 이 Metrics의 processing_ms는
     *
     * Frame -> GstBuffer Copy
     * +
     * appsrc Push
     *
     * 시간이다.
     *
     * H.264 Encoder 실제 처리시간이 아니다.
     */
    std::vector<StageMetric> submission_metrics_;
    std::vector<FpsMetric> fps_metrics_;
    std::uint64_t success_count_ = 0;
    std::uint64_t failure_count_ = 0;
    double fps_duration_seconds_ = 0.0;
};
