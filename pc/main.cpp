#include "receiver.hpp"

#include <iostream>
#include <string>


int main(int argc, char* argv[])
{
    constexpr int kPort = 5004;
    const bool measurement_enabled = true;

    // 성능 측정 실행에서는 false, 데모 녹화 실행에서는 true.
    const bool recording_enabled = false;

    // Baseline / Optimized 실행에 맞게 파일명만 변경한다.
    // Timestamp CSV는 같은 위치에 *_timestamps.csv로 저장된다.
    const std::string output_path =
        "results/baseline.mp4";

    const double recording_fps = 30.0;

    bool clocks_synchronized = false;

    if (argc == 2 &&
        std::string(argv[1]) ==
            "--clocks-synchronized")
    {
        // Raspberry Pi와 PC 양쪽의 PTP/NTP 동기화를 확인한 경우에만 사용.
        clocks_synchronized = true;
    }
    else if (argc != 1)
    {
        std::cerr
            << "Usage: " << argv[0]
            << " [--clocks-synchronized]\n";

        return 1;
    }

    Receiver receiver(
        kPort,
        measurement_enabled,
        clocks_synchronized,
        recording_enabled,
        output_path,
        recording_fps
    );

    if (!receiver.open())
    {
        std::cerr
            << "[Main] Receiver 시작 실패\n";

        return 1;
    }

    receiver.run();

    return 0;
}
