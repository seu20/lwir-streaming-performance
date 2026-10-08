#pragma once

#include <pthread.h>
#include <vector>

#include "app/preprocess.hpp"
#include "common/frame.hpp"
#include "common/stage_metric.hpp"
#include "common/threadsafequeue.hpp"


class PreprocessThread
{
public:
    PreprocessThread(
        Preprocess& preprocess,
        ThreadSafeQueue<Frame>& input_queue,
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

    Preprocess& preprocess_;

    ThreadSafeQueue<Frame>& input_queue_;
    ThreadSafeQueue<Frame>& output_queue_;

    bool measurement_enabled_;
    pthread_t thread_{};

    std::vector<StageMetric> metrics_;
};