#pragma once

#include "app/capture.hpp"
#include "app/preprocess.hpp"
#include "app/streaming.hpp"

#include "pipeline/capture_thread.hpp"
#include "pipeline/preprocess_thread.hpp"
#include "pipeline/streaming_thread.hpp"

#include "common/frame.hpp"
#include "common/threadsafequeue.hpp"


class Application
{
public:
    Application(
        const CaptureConfig& capture_config,
        const StreamingConfig& streaming_config,
        bool measurement_enabled);

    bool run();

private:
    Capture capture_;
    Preprocess preprocess_;
    Streaming streaming_;

    ThreadSafeQueue<Frame> capture_queue_;
    ThreadSafeQueue<Frame> streaming_queue_;

    CaptureThread capture_thread_;
    PreprocessThread preprocess_thread_;
    StreamingThread streaming_thread_;
    bool measurement_enabled_;
};
