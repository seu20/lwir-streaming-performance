#pragma once

#include <chrono>
#include <cstdint>

// 같은 프레임을 파이프라인 전체에서 식별하고
// E2E / Queue Wait 측정에 필요한 최소 정보만 유지한다.
struct Metadata
{
    std::uint64_t frame_id = 0;

    // Camera Capture가 완료된 시점.
    // 최종 E2E Latency 계산을 위해 끝까지 유지한다.
    std::chrono::steady_clock::time_point captured_at{};

    // 현재 Stage 앞 Queue에 들어간 시점.
    // 다음 Stage가 시작되면 Queue Wait 계산에 사용하고,
    // 다음 Queue에 넣기 직전에 새로운 값으로 덮어쓴다.
    std::chrono::steady_clock::time_point enqueued_at{};
};