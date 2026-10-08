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
};