#include "app/preprocess.hpp"

#include <opencv2/core.hpp>

#include "common/logger.hpp"


bool Preprocess::process(Frame& frame)
{
    if (frame.image.empty())
    {
        Logger::error("[Preprocess] 입력 Frame이 비어 있습니다");
        return false;
    }

    if (frame.image.type() != CV_16UC1)
    {
        Logger::error("[Preprocess] 입력 Frame 형식이 CV_16UC1이 아닙니다");
        return false;
    }

    cv::Mat image_8bit;

    // 16-bit unsigned grayscale의 전체 범위(0~65535)를
    // 8-bit unsigned grayscale 범위(0~255)로 고정 변환한다.
    frame.image.convertTo(
        image_8bit,
        CV_8UC1,
        1.0 / 256.0
    );

    frame.image = std::move(image_8bit);

    return true;
}