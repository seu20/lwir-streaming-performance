#pragma once

#include "common/frame.hpp"


class Preprocess
{
public:
    // LWIR 원본 Frame을 Encoding 가능한 8-bit Gray 영상으로 변환한다.
    bool process(Frame& frame);
};