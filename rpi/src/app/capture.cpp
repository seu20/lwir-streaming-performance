#include "app/capture.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "common/logger.hpp"
#include "i3system_TE.h"


namespace
{
// 파일명의 숫자 부분은 숫자값으로 비교하여
// frame_2.png가 frame_10.png보다 먼저 오도록 정렬한다.
bool natural_filename_less(
    const std::string& left,
    const std::string& right)
{
    std::size_t left_index = 0;
    std::size_t right_index = 0;

    while (left_index < left.size() &&
           right_index < right.size())
    {
        const auto left_character =
            static_cast<unsigned char>(left[left_index]);
        const auto right_character =
            static_cast<unsigned char>(right[right_index]);

        if (std::isdigit(left_character) &&
            std::isdigit(right_character))
        {
            std::size_t left_zero_end = left_index;
            std::size_t right_zero_end = right_index;

            while (left_zero_end < left.size() &&
                   left[left_zero_end] == '0')
            {
                ++left_zero_end;
            }

            while (right_zero_end < right.size() &&
                   right[right_zero_end] == '0')
            {
                ++right_zero_end;
            }

            std::size_t left_digit_end = left_zero_end;
            std::size_t right_digit_end = right_zero_end;

            while (left_digit_end < left.size() &&
                   std::isdigit(static_cast<unsigned char>(
                       left[left_digit_end])))
            {
                ++left_digit_end;
            }

            while (right_digit_end < right.size() &&
                   std::isdigit(static_cast<unsigned char>(
                       right[right_digit_end])))
            {
                ++right_digit_end;
            }

            const auto left_digit_count =
                left_digit_end - left_zero_end;
            const auto right_digit_count =
                right_digit_end - right_zero_end;

            if (left_digit_count != right_digit_count)
                return left_digit_count < right_digit_count;

            const int digit_compare = left.compare(
                left_zero_end,
                left_digit_count,
                right,
                right_zero_end,
                right_digit_count
            );

            if (digit_compare != 0)
                return digit_compare < 0;

            const auto left_total_digit_count =
                left_digit_end - left_index;
            const auto right_total_digit_count =
                right_digit_end - right_index;

            if (left_total_digit_count !=
                right_total_digit_count)
            {
                return left_total_digit_count <
                    right_total_digit_count;
            }

            left_index = left_digit_end;
            right_index = right_digit_end;
            continue;
        }

        const auto normalized_left =
            static_cast<unsigned char>(std::tolower(left_character));
        const auto normalized_right =
            static_cast<unsigned char>(std::tolower(right_character));

        if (normalized_left != normalized_right)
            return normalized_left < normalized_right;

        ++left_index;
        ++right_index;
    }

    return left.size() < right.size();
}
}


Capture::Capture(
    CaptureConfig config,
    int expected_width,
    int expected_height)
    : config_(std::move(config)),
      expected_width_(expected_width),
      expected_height_(expected_height)
{
}


Capture::~Capture()
{
    close();
}


// Capture를 초기화한다.
bool Capture::open()
{
    close();

    if (config_.input_mode == InputMode::Dataset)
        return open_dataset();

    return open_camera();
}


bool Capture::open_camera()
{
    Logger::info("[Capture] TE-EV1 열기 시도");

    // TE-EV1 열기. devnum_ 은 0으로 고정
    te_ = i3::OpenTE_A(0);

    if (!te_)
    {
        Logger::error("[Capture] TE-EV1 열기 실패");
        return false;
    }

    width_ = te_->GetImageWidth();
    height_ = te_->GetImageHeight();
    if (width_ <= 0 || height_ <= 0)
    {
        Logger::error("[Capture] 유효하지 않은 영상 크기");
        close();
        return false;
    }

    return true;
}


bool Capture::capture(Frame& frame)
{
    if (config_.input_mode == InputMode::Dataset)
        return capture_dataset(frame);

    return capture_camera(frame);
}


bool Capture::capture_camera(Frame& frame)
{
    if (!te_ || width_ <= 0 || height_ <= 0)
        return false;

    // SDK가 애플리케이션 소유 버퍼에 직접 기록하므로 FrameContext가 영상
    // 수명을 소유한다. 실제 장치 크기로 할당해 버퍼 오버런을 방지한다.
    frame.image.create(height_, width_, CV_16UC1);
    auto* buffer = reinterpret_cast<unsigned short*>(frame.image.data);

    // 학습/전처리의 고정 16-bit 입력 범위를 유지하기 위해 SDK AGC를 끈다.
    const int receive_result = te_->RecvImage(buffer, false);
    return receive_result == 1;
}


bool Capture::open_dataset()
{
    if (config_.input_fps <= 0)
    {
        Logger::error("[Capture] Dataset input_fps는 1 이상이어야 합니다");
        return false;
    }

    if (config_.dataset_path.empty())
    {
        Logger::error("[Capture] Dataset 경로가 비어 있습니다");
        return false;
    }

    const std::filesystem::path dataset_path(
        config_.dataset_path
    );
    std::error_code error;

    if (!std::filesystem::exists(dataset_path, error) ||
        error)
    {
        Logger::error(
            "[Capture] Dataset 경로가 존재하지 않습니다: " +
            config_.dataset_path
        );
        return false;
    }

    if (!std::filesystem::is_directory(dataset_path, error) ||
        error)
    {
        Logger::error(
            "[Capture] Dataset 경로가 디렉터리가 아닙니다: " +
            config_.dataset_path
        );
        return false;
    }

    dataset_files_.clear();
    dataset_index_ = 0;

    std::filesystem::directory_iterator iterator(
        dataset_path,
        error
    );
    const std::filesystem::directory_iterator end;

    while (!error && iterator != end)
    {
        const auto& entry = *iterator;

        if (entry.is_regular_file(error) && !error)
        {
            std::string extension =
                entry.path().extension().string();

            std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(
                        std::tolower(character)
                    );
                }
            );

            if (extension == ".png")
                dataset_files_.push_back(entry.path().string());
        }

        iterator.increment(error);
    }

    if (error)
    {
        Logger::error(
            "[Capture] Dataset 디렉터리 읽기 실패: " +
            error.message()
        );
        dataset_files_.clear();
        return false;
    }

    if (dataset_files_.empty())
    {
        Logger::error(
            "[Capture] Dataset 경로에 PNG 파일이 없습니다: " +
            config_.dataset_path
        );
        return false;
    }

    std::sort(
        dataset_files_.begin(),
        dataset_files_.end(),
        [](const std::string& left, const std::string& right)
        {
            return natural_filename_less(
                std::filesystem::path(left).filename().string(),
                std::filesystem::path(right).filename().string()
            );
        }
    );

    Logger::info(
        "[Capture] Dataset 열기 완료: " +
        std::to_string(dataset_files_.size()) +
        "개 PNG, " +
        std::to_string(config_.input_fps) +
        " FPS"
    );

    return true;
}


bool Capture::capture_dataset(Frame& frame)
{
    if (dataset_index_ >= dataset_files_.size())
        return false;

    const std::string& filename =
        dataset_files_[dataset_index_];

    // IMREAD_UNCHANGED로 원본 16-bit 값을 변환 없이 읽는다.
    cv::Mat image = cv::imread(
        filename,
        cv::IMREAD_UNCHANGED
    );

    if (image.empty())
    {
        Logger::error(
            "[Capture] Dataset PNG 읽기 실패: " + filename
        );
        return false;
    }

    if (image.type() != CV_16UC1)
    {
        Logger::error(
            "[Capture] Dataset PNG가 CV_16UC1 형식이 아닙니다: " +
            filename
        );
        return false;
    }

    if (dataset_index_ == 0)
    {
        if ((expected_width_ > 0 &&
             image.cols != expected_width_) ||
            (expected_height_ > 0 &&
             image.rows != expected_height_))
        {
            Logger::error(
                "[Capture] Dataset PNG 해상도가 Streaming 설정과 "
                "다릅니다: " + filename
            );
            return false;
        }

        width_ = image.cols;
        height_ = image.rows;
    }
    else if (image.cols != width_ || image.rows != height_)
    {
        Logger::error(
            "[Capture] Dataset PNG 해상도가 첫 프레임과 다릅니다: " +
            filename
        );
        return false;
    }

    frame.image = std::move(image);
    ++dataset_index_;
    return true;
}


// Capture 자원을 해제한다.
void Capture::close()
{
    if (te_)
    {
        te_->CloseTE();
        te_ = nullptr;
        Logger::info("[Capture] TE-EV1 닫기 완료");
    }

    dataset_files_.clear();
    dataset_index_ = 0;
    width_ = 0;
    height_ = 0;
}
