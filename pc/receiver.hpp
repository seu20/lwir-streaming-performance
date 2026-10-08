#pragma once

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

class Receiver
{
public:
    Receiver(int port);
    ~Receiver();

    bool open();
    void run();
    void close();

private:
    int port_;

    GstElement* pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;

    bool opened_ = false;
};