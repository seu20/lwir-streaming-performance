#pragma once

#include <string>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "common/frame.hpp"


struct StreamingConfig
{
    std::string host;

    int port = 5004;

    int width = 640;
    int height = 480;
    int fps = 30;
};


class Streaming
{
public:
    explicit Streaming(StreamingConfig config);
    ~Streaming();

    // GStreamer Pipeline을 생성하고 PLAYING 상태로 전환한다.
    bool open();

    // 전처리가 끝난 CV_8UC1 Frame을 appsrc에 전달한다.
    bool push(const Frame& frame);

    // EOS 전달 후 GStreamer 자원을 해제한다.
    void close();

private:
    StreamingConfig config_;

    GstElement* pipeline_ = nullptr;

    GstElement* appsrc_ = nullptr;
    GstElement* queue_ = nullptr;
    GstElement* encoder_ = nullptr;
    GstElement* parser_ = nullptr;
    GstElement* payloader_ = nullptr;
    GstElement* sink_ = nullptr;

    bool opened_ = false;
};