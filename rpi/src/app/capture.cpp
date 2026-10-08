#include "app/capture.hpp"
#include <opencv2/core.hpp>
#include "common/logger.hpp"
#include "i3system_TE.h"


Capture::~Capture()
{
    close();
}


// Capture를 초기화한다.
bool Capture::open()
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


// Capture 자원을 해제한다.
void Capture::close()
{
    if (!te_)
        return;

    te_->CloseTE();
    te_ = nullptr;
    width_ = 0;
    height_ = 0;
    Logger::info("[Capture] 닫기 완료");
}
