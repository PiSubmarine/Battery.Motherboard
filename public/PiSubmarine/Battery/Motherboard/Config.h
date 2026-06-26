#pragma once

#include "PiSubmarine/Max17261/Device.h"
#include <chrono>
#include <thread>

namespace PiSubmarine::Battery::Motherboard
{
    struct Config
    {
        Max17261::MicroAmpereHours DesignCapacity{};
        Max17261::MicroAmperes ChargeTerminationCurrent{};
        Max17261::MicroVolts EmptyVoltage{};
        std::chrono::nanoseconds RefreshInterval{std::chrono::seconds(1)};
        Max17261::WaitFunc WaitFunction = [](const std::chrono::milliseconds duration)
        {
            std::this_thread::sleep_for(duration);
        };
        bool ForceGaugeReset = false;
    };
}
