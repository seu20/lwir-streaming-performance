#include "app/application.hpp"
#include "common/logger.hpp"


int main()
{
    // 입력 설정: InputMode::Camera 또는 InputMode::Dataset.
    const InputMode input_mode = InputMode::Dataset;

    // Dataset 모드에서 사용할 16-bit PNG 디렉터리.
    // 절대경로와 실행 위치 기준 상대경로를 모두 사용할 수 있다.
    const std::string dataset_path =
        "/workspace/dataset/seq_001/images";

    // Dataset 공급 FPS이며 Streaming caps에도 같은 값을 사용한다.
    const int input_fps = 30;

    CaptureConfig capture_config;
    capture_config.input_mode = input_mode;
    capture_config.dataset_path = dataset_path;
    capture_config.input_fps = input_fps;

    StreamingConfig streaming_config;

    // Raspberry Pi에서 영상을 받을 PC의 IP.
    streaming_config.host = "192.168.0.10";

    streaming_config.port = 5004;

    // TE-EV1 / Preprocess 출력 조건.
    streaming_config.width = 640;
    streaming_config.height = 480;
    streaming_config.fps = input_fps;

    const bool measurement_enabled = true;


    Application app(
        capture_config,
        streaming_config,
        measurement_enabled
    );


    if (!app.run())
    {
        Logger::error("[Main] Application 실행 실패");
        return 1;
    }


    return 0;
}
