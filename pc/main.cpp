#include "receiver.hpp"

#include <iostream>


int main()
{
    constexpr int kPort = 5004;

    Receiver receiver(kPort);

    if (!receiver.open())
    {
        std::cerr
            << "[Main] Receiver 시작 실패\n";

        return 1;
    }

    receiver.run();

    return 0;
}