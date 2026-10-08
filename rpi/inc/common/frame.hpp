#pragma once

#include <opencv2/core.hpp>

#include "common/metadata.hpp"


struct Frame
{
    Metadata metadata;

    // Capture: CV_16UC1
    // Preprocess 이후: CV_8UC1
    cv::Mat image;
};