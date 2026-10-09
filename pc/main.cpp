#include "receiver.hpp"

#include <iostream>
#include <string>


namespace
{
bool read_value(
    int argc,
    char* argv[],
    int& index,
    std::string& value)
{
    if (index + 1 >= argc)
        return false;

    value = argv[++index];
    return true;
}


void print_usage(const char* executable)
{
    std::cerr
        << "Usage: " << executable
        << " [--clocks-synchronized] [--condition ID]"
        << " [--output-dir PATH] [--idle-timeout SEC]"
        << " [--record-output MP4]\n";
}
}


int main(int argc, char* argv[])
{
    ReceiverConfig config;
    std::string condition = "B0";

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        std::string value;

        if (argument == "--clocks-synchronized")
        {
            config.clocks_synchronized = true;
        }
        else if (argument == "--condition" &&
                 read_value(argc, argv, index, value))
        {
            condition = value;
        }
        else if (argument == "--output-dir" &&
                 read_value(argc, argv, index, value))
        {
            config.metrics_output_dir = value;
        }
        else if (argument == "--idle-timeout" &&
                 read_value(argc, argv, index, value))
        {
            config.idle_timeout_seconds =
                std::stod(value);
        }
        else if (argument == "--record-output" &&
                 read_value(argc, argv, index, value))
        {
            config.recording_enabled = true;
            config.recording_output_path = value;
        }
        else
        {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (condition == "N1")
        config.udp_buffer_bytes = 65536;
    else if (condition == "N2-0")
        config.jitter_latency_ms = 0;
    else if (condition == "N2-20")
        config.jitter_latency_ms = 20;
    else if (condition == "N2-50")
        config.jitter_latency_ms = 50;

    Receiver receiver(config);

    if (!receiver.open())
    {
        std::cerr
            << "[Main] Receiver 시작 실패\n";

        return 1;
    }

    receiver.run();

    return 0;
}
