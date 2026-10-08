#pragma once

#include "common/frame.hpp"

namespace i3
{
class TE_A;
}

class Capture
{
private:
    i3::TE_A* te_ = nullptr;
    int width_ = 0;
    int height_ = 0;

public:
    Capture() = default;
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // 카메라 사용 시작
    bool open();
    // 프레임 읽기
    bool capture(Frame& frame);
    // 카메라 사용 종료
    void close();
};
