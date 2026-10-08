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
private:
    static void* thread_func(void* arg);
    void run();

    Capture& capture_;
    ThreadSafeQueue<Frame>& output_queue_;

    bool measurement_enabled_;
    pthread_t thread_{};

    std::vector<StageMetric> metrics_;
};