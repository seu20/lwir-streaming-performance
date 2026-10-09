#include "app/application.hpp"
#include "common/logger.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
struct Options
{
    std::string condition = "B0";
    std::string dataset_path = "/workspace/dataset/seq_001/images";
    std::string host = "10.42.0.1";
    std::string output_dir;
    int fps = 30;
};

bool parse_options(int argc, char* argv[], Options& options)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--help")
            return false;
        if (index + 1 >= argc)
        {
            std::cerr << "Missing value for " << argument << '\n';
            return false;
        }
        const std::string value = argv[++index];
        if (argument == "--condition")
            options.condition = value;
        else if (argument == "--dataset-path")
            options.dataset_path = value;
        else if (argument == "--host")
            options.host = value;
        else if (argument == "--output-dir")
            options.output_dir = value;
        else if (argument == "--fps")
            options.fps = std::stoi(value);
        else
        {
            std::cerr << "Unknown option: " << argument << '\n';
            return false;
        }
    }
    return options.fps > 0;
}

bool apply_condition(
    const std::string& condition,
    QueueConfig& queue,
    StreamingConfig& streaming)
{
    if (condition == "B0" || condition == "Q0" ||
        condition == "E0" || condition == "G0" ||
        condition == "P0" || condition == "N0" ||
        condition == "N1" || condition == "N2-0" ||
        condition == "N2-20" || condition == "N2-50" ||
        condition.rfind("N3-", 0) == 0)
        return true;
    if (condition == "Q1")
        queue.max_size = 2;
    else if (condition == "Q2")
        queue.max_size = 2;
    else if (condition == "Q3")
    {
        queue.max_size = 1;
        queue.policy = QueuePolicy::Latest;
    }
    else if (condition == "E1")
        streaming.tune = "zerolatency";
    else if (condition == "B1")
    {
        // E1 and E2 both produced repeatable independent improvements.
        // Q1 did not, so the final combination contains encoder changes only.
        streaming.tune = "zerolatency";
        streaming.speed_preset = "ultrafast";
    }
    else if (condition == "E2")
        streaming.speed_preset = "ultrafast";
    else if (condition == "E3")
        streaming.bframes = 0;
    else if (condition == "E5-1000")
        streaming.bitrate_kbps = 1000;
    else if (condition == "E5-2000")
        streaming.bitrate_kbps = 2000;
    else if (condition == "E5-4000")
        streaming.bitrate_kbps = 4000;
    else if (condition == "E6-15")
        streaming.key_int_max = 15;
    else if (condition == "E6-30")
        streaming.key_int_max = 30;
    else if (condition == "E6-60")
        streaming.key_int_max = 60;
    else if (condition == "G1")
        streaming.gst_queue_depth = 2;
    else if (condition == "G2")
    {
        streaming.gst_queue_depth = 2;
        streaming.gst_queue_leaky = 2;
    }
    else if (condition == "G3")
        streaming.direct_gray8 = true;
    else if (condition == "P1")
        streaming.zero_copy_submit = true;
    else
        return false;
    return true;
}

void print_usage(const char* executable)
{
    std::cerr
        << "Usage: " << executable
        << " [--condition ID] [--dataset-path PATH]"
        << " [--host IP] [--fps N] [--output-dir PATH]\n";
}
}

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options))
    {
        print_usage(argv[0]);
        return 1;
    }
    // 입력 설정: InputMode::Camera 또는 InputMode::Dataset.
    const InputMode input_mode = InputMode::Dataset;

    // Dataset 모드에서 사용할 16-bit PNG 디렉터리.
    // 절대경로와 실행 위치 기준 상대경로를 모두 사용할 수 있다.
    const std::string dataset_path =
        options.dataset_path;

    // Dataset 공급 FPS이며 Streaming caps에도 같은 값을 사용한다.
    const int input_fps = options.fps;

    CaptureConfig capture_config;
    capture_config.input_mode = input_mode;
    capture_config.dataset_path = dataset_path;
    capture_config.input_fps = input_fps;

    StreamingConfig streaming_config;

    // Raspberry Pi에서 영상을 받을 PC의 IP.
    streaming_config.host = options.host;

    streaming_config.port = 5004;

    // TE-EV1 / Preprocess 출력 조건.
    streaming_config.width = 640;
    streaming_config.height = 480;
    streaming_config.fps = input_fps;

    const bool measurement_enabled = true;

    QueueConfig queue_config;
    if (!apply_condition(
            options.condition,
            queue_config,
            streaming_config))
    {
        Logger::error(
            "[Main] 지원하지 않는 실험 조건: " +
            options.condition
        );
        return 1;
    }

    Logger::info(
        "[Main] 실험 조건=" + options.condition
    );


    Application app(
        capture_config,
        streaming_config,
        measurement_enabled,
        queue_config,
        options.output_dir
    );


    if (!app.run())
    {
        Logger::error("[Main] Application 실행 실패");
        return 1;
    }


    return 0;
}
