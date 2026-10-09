#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common/frame.hpp"

namespace i3
{
class TE_A;
}

enum class InputMode
{
    Camera,
    Dataset
};

struct CaptureConfig
{
    InputMode input_mode = InputMode::Camera;
    std::string dataset_path;
    int input_fps = 30;
};

class Capture
{
private:
    CaptureConfig config_;
    i3::TE_A* te_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int expected_width_ = 0;
    int expected_height_ = 0;
    std::vector<std::string> dataset_files_;
    std::size_t dataset_index_ = 0;

    bool open_camera();
    bool open_dataset();
    bool capture_camera(Frame& frame);
    bool capture_dataset(Frame& frame);

public:
    explicit Capture(
        CaptureConfig config = {},
        int expected_width = 0,
        int expected_height = 0);
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // 선택한 입력 소스 사용 시작
    bool open();
    // 프레임 읽기
    bool capture(Frame& frame);
    // 입력 소스 사용 종료
    void close();

    bool is_dataset_mode() const
    {
        return config_.input_mode == InputMode::Dataset;
    }

    int input_fps() const
    {
        return config_.input_fps;
    }
};
