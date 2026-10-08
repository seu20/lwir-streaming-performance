#include "app/application.hpp"
#include "common/logger.hpp"


int main()
{
    StreamingConfig streaming_config;

    // Raspberry Pi에서 영상을 받을 PC의 IP.
    streaming_config.host = "192.168.0.10";

    streaming_config.port = 5004;

    // TE-EV1 / Preprocess 출력 조건.
    streaming_config.width = 640;
    streaming_config.height = 480;
    streaming_config.fps = 30;

    const bool measurement_enabled = true;


    Application app(
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