#pragma once

#include "PiSubmarine/Battery/Motherboard/Config.h"
#include "PiSubmarine/Battery/Telemetry/Api/IProvider.h"
#include "PiSubmarine/Time/ITickable.h"
#include <memory>
#include <optional>
#include <spdlog/fwd.h>

namespace PiSubmarine::Battery::Persistence::Api
{
    class IStore;
}

namespace PiSubmarine::Chipset
{
    class IClient;
}

namespace PiSubmarine::Logging::Api
{
    class IFactory;
}

namespace PiSubmarine::Max17261
{
    class Device;
    struct AlgorithmLearningParameters;
}

namespace PiSubmarine::Battery::Motherboard
{
    class Provider final : public Telemetry::Api::IProvider, public Time::ITickable
    {
    public:
        Provider(
            Max17261::Device& device,
            Chipset::IClient& chipsetClient,
            Persistence::Api::IStore& store,
            Logging::Api::IFactory& loggerFactory,
            Config config = {});

        [[nodiscard]] Error::Api::Result<void> Initialize();
        [[nodiscard]] Error::Api::Result<Telemetry::Api::State> GetState() const override;
        void Tick(const std::chrono::nanoseconds& uptime, const std::chrono::nanoseconds& deltaTime) override;

    private:
        Max17261::Device& m_Device;
        Chipset::IClient& m_ChipsetClient;
        Persistence::Api::IStore& m_Store;
        Config m_Config;
        std::shared_ptr<spdlog::logger> m_Logger;
        std::chrono::nanoseconds m_NextRefreshTime{0};
        std::optional<Error::Api::Error> m_LastReadError;
        Telemetry::Api::State m_State{};
        bool m_IsInitialized = false;
        bool m_HasState = false;
        bool m_HasPersistedCycleBit = false;
        bool m_LastPersistedCycleBit = false;

        [[nodiscard]] Error::Api::Result<void> RefreshState();
        [[nodiscard]] Error::Api::Result<void> MaybePersistLearningParameters();
        [[nodiscard]] Error::Api::Result<void> RestorePersistedLearningParameters();
    };
}
