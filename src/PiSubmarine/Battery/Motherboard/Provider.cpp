#include "PiSubmarine/Battery/Motherboard/Provider.h"

#include "PiSubmarine/Battery/Persistence/Api/IStore.h"
#include "PiSubmarine/Chipset/Api/IClient.h"
#include "PiSubmarine/Error/Api/MakeError.h"
#include "PiSubmarine/Exceptions.h"
#include "PiSubmarine/Logging/Api/IFactory.h"
#include "PiSubmarine/Max17261/Device.h"
#include <array>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <vector>

namespace PiSubmarine::Battery::Motherboard
{
    namespace
    {
        using Error::Api::ErrorCondition;
        using Error::Api::MakeError;
        using Error::Api::Result;

        constexpr std::uint32_t PersistencePayloadVersion = 1;
        constexpr std::size_t PersistenceHeaderSize = sizeof(std::uint32_t) * 2;
        constexpr std::uint16_t PredictionUnavailableRaw = 0xFFFF;
        constexpr std::uint16_t PersistenceToggleMask = 1u << 6;
        constexpr std::chrono::milliseconds PredictionLsb{5625};

        [[nodiscard]] std::shared_ptr<spdlog::logger> CreateLogger(Logging::Api::IFactory& loggerFactory)
        {
            return loggerFactory.CreateLogger("Battery.Motherboard");
        }

        [[nodiscard]] constexpr bool HasPersistenceToggleBit(const std::uint16_t cycles)
        {
            return (cycles & PersistenceToggleMask) != 0;
        }

        [[nodiscard]] constexpr std::uint32_t ReadU32(const std::span<const std::byte> data, const std::size_t offset)
        {
            return std::to_integer<std::uint32_t>(data[offset + 0])
                | (std::to_integer<std::uint32_t>(data[offset + 1]) << 8)
                | (std::to_integer<std::uint32_t>(data[offset + 2]) << 16)
                | (std::to_integer<std::uint32_t>(data[offset + 3]) << 24);
        }

        void WriteU32(std::vector<std::byte>& data, const std::uint32_t value)
        {
            data.push_back(static_cast<std::byte>(value & 0xFFu));
            data.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
            data.push_back(static_cast<std::byte>((value >> 16) & 0xFFu));
            data.push_back(static_cast<std::byte>((value >> 24) & 0xFFu));
        }

        [[nodiscard]] std::optional<std::chrono::milliseconds> DecodePrediction(const std::uint16_t raw)
        {
            if (raw == PredictionUnavailableRaw)
            {
                return std::nullopt;
            }

            return std::chrono::milliseconds{PredictionLsb.count() * raw};
        }

        [[nodiscard]] Result<Max17261::AlgorithmLearningParameters> DeserializeLearningParameters(
            const std::span<const std::byte> payload)
        {
            if (payload.size() < PersistenceHeaderSize)
            {
                return std::unexpected(MakeError(ErrorCondition::ContractError));
            }

            const auto version = ReadU32(payload, 0);
            const auto serializedSize = ReadU32(payload, sizeof(std::uint32_t));
            if (version != PersistencePayloadVersion
                || serializedSize != Max17261::AlgorithmLearningParameters::SerializedSize
                || payload.size() != PersistenceHeaderSize + serializedSize)
            {
                return std::unexpected(MakeError(ErrorCondition::ContractError));
            }

            std::array<std::uint8_t, Max17261::AlgorithmLearningParameters::SerializedSize> rawData{};
            for (std::size_t index = 0; index < rawData.size(); ++index)
            {
                rawData[index] = std::to_integer<std::uint8_t>(payload[PersistenceHeaderSize + index]);
            }

            return Max17261::AlgorithmLearningParameters::Deserialize(rawData);
        }

        [[nodiscard]] std::vector<std::byte> SerializeLearningParameters(
            const Max17261::AlgorithmLearningParameters& parameters)
        {
            const auto serialized = parameters.Serialize();
            std::vector<std::byte> payload;
            payload.reserve(PersistenceHeaderSize + serialized.size());
            WriteU32(payload, PersistencePayloadVersion);
            WriteU32(payload, static_cast<std::uint32_t>(serialized.size()));
            for (const auto value : serialized)
            {
                payload.push_back(static_cast<std::byte>(value));
            }
            return payload;
        }
    }

    Provider::Provider(
        Max17261::Device& device,
        Chipset::IClient& chipsetClient,
        Persistence::Api::IStore& store,
        Logging::Api::IFactory& loggerFactory)
        : Provider(device, chipsetClient, store, loggerFactory, Config{})
    {
    }

    Provider::Provider(
        Max17261::Device& device,
        Chipset::IClient& chipsetClient,
        Persistence::Api::IStore& store,
        Logging::Api::IFactory& loggerFactory,
        Config config) :
        m_Device(device),
        m_ChipsetClient(chipsetClient),
        m_Store(store),
        m_Config(config),
        m_Logger(CreateLogger(loggerFactory))
    {
    }

    Result<void> Provider::Initialize()
    {
        if (auto result = m_Device.Init(
            m_Config.WaitFunction,
            m_Config.DesignCapacity,
            m_Config.ChargeTerminationCurrent,
            m_Config.EmptyVoltage,
            m_Config.ForceGaugeReset); !result.has_value())
        {
            m_IsInitialized = false;
            m_HasState = false;
            m_LastReadError = result.error();
            return std::unexpected(result.error());
        }

        if (auto result = RestorePersistedLearningParameters(); !result.has_value())
        {
            return result;
        }

        if (auto result = RefreshState(); !result.has_value())
        {
            return result;
        }

        auto cycleCountResult = m_Device.GetCycleCount();
        if (!cycleCountResult.has_value())
        {
            m_LastReadError = cycleCountResult.error();
            return std::unexpected(cycleCountResult.error());
        }

        m_LastPersistedCycleBit = HasPersistenceToggleBit(*cycleCountResult);
        m_HasPersistedCycleBit = true;
        m_IsInitialized = true;
        m_NextRefreshTime = std::chrono::nanoseconds{0};
        return {};
    }

    Result<Telemetry::Api::State> Provider::GetState() const
    {
        if (!m_IsInitialized)
        {
            return std::unexpected(MakeError(ErrorCondition::ContractError));
        }

        if (m_LastReadError.has_value())
        {
            return std::unexpected(*m_LastReadError);
        }

        if (!m_HasState)
        {
            return std::unexpected(MakeError(ErrorCondition::NotReady));
        }

        return m_State;
    }

    void Provider::Tick(const std::chrono::nanoseconds& uptime, const std::chrono::nanoseconds&)
    {
        if (!m_IsInitialized)
        {
            SPDLOG_LOGGER_CRITICAL(m_Logger, "Battery.Motherboard.Provider::Tick() called before Initialize()");
            Exceptions::Throw(std::logic_error("Battery.Motherboard.Provider::Tick() called before Initialize()"));
        }

        if (uptime < m_NextRefreshTime)
        {
            return;
        }

        if (auto result = RefreshState(); !result.has_value())
        {
            m_NextRefreshTime = uptime + m_Config.RefreshInterval;
            return;
        }

        if (auto result = MaybePersistLearningParameters(); !result.has_value())
        {
            SPDLOG_LOGGER_WARN(m_Logger, "Failed to persist battery learning parameters: {}", static_cast<int>(result.error().Condition));
        }

        m_NextRefreshTime = uptime + m_Config.RefreshInterval;
    }

    Result<void> Provider::RefreshState()
    {
		// MAX17261 reports average voltage of a single cell.
        const auto packVoltageResult = m_Device.GetInstantVoltage();
        if (!packVoltageResult.has_value())
        {
            m_LastReadError = packVoltageResult.error();
            return std::unexpected(packVoltageResult.error());
        }

        auto packVolts = Volts(packVoltageResult->ToVolts().Value * m_Config.CellsNum);

        const auto packCurrentResult = m_Device.GetInstantCurrent();
        if (!packCurrentResult.has_value())
        {
            m_LastReadError = packCurrentResult.error();
            return std::unexpected(packCurrentResult.error());
        }

        const auto remainingCapacityResult = m_Device.GetRemainingCapacityEstimate();
        if (!remainingCapacityResult.has_value())
        {
            m_LastReadError = remainingCapacityResult.error();
            return std::unexpected(remainingCapacityResult.error());
        }

        const auto stateOfChargeResult = m_Device.GetRemainingStateOfCharge();
        if (!stateOfChargeResult.has_value())
        {
            m_LastReadError = stateOfChargeResult.error();
            return std::unexpected(stateOfChargeResult.error());
        }

        const auto timeToFullResult = m_Device.GetTimeToFullRaw();
        if (!timeToFullResult.has_value())
        {
            m_LastReadError = timeToFullResult.error();
            return std::unexpected(timeToFullResult.error());
        }

        const auto timeToEmptyResult = m_Device.GetTimeToEmptyRaw();
        if (!timeToEmptyResult.has_value())
        {
            m_LastReadError = timeToEmptyResult.error();
            return std::unexpected(timeToEmptyResult.error());
        }

        const auto monitorTemperatureResult = m_Device.GetDieTemperature();
        if (!monitorTemperatureResult.has_value())
        {
            m_LastReadError = monitorTemperatureResult.error();
            return std::unexpected(monitorTemperatureResult.error());
        }

        const auto chargerVoltageResult = m_ChipsetClient.GetChargerBusVoltage();
        if (!chargerVoltageResult.has_value())
        {
            m_LastReadError = chargerVoltageResult.error();
            return std::unexpected(chargerVoltageResult.error());
        }

        const auto chargerCurrentResult = m_ChipsetClient.GetChargerBusCurrent();
        if (!chargerCurrentResult.has_value())
        {
            m_LastReadError = chargerCurrentResult.error();
            return std::unexpected(chargerCurrentResult.error());
        }

        const auto chargerTemperatureResult = m_ChipsetClient.GetChargerTemperature();
        if (!chargerTemperatureResult.has_value())
        {
            m_LastReadError = chargerTemperatureResult.error();
            return std::unexpected(chargerTemperatureResult.error());
        }

        const auto packTemperatureResult = m_ChipsetClient.GetBatteryTemperature();
        if (!packTemperatureResult.has_value())
        {
            m_LastReadError = packTemperatureResult.error();
            return std::unexpected(packTemperatureResult.error());
        }

        m_State = Telemetry::Api::State{
            .PackVoltage = packVolts,
            .ChargerVoltage = *chargerVoltageResult,
            .PackCurrent = packCurrentResult->ToAmperes(),
            .ChargerCurrent = *chargerCurrentResult,
            .ChargerTemperature = *chargerTemperatureResult,
            .PackTemperature = *packTemperatureResult,
            .MonitorTemperature = monitorTemperatureResult->ToCelsius(),
            .RemainingCapacity = remainingCapacityResult->ToAmpereHours(),
            .StateOfCharge = NormalizedFraction{stateOfChargeResult->ToFactor()},
            .TimeToFull = DecodePrediction(*timeToFullResult),
            .TimeToEmpty = DecodePrediction(*timeToEmptyResult)
        };

        m_HasState = true;
        m_LastReadError.reset();
        return {};
    }

    Result<void> Provider::MaybePersistLearningParameters()
    {
        const auto cycleCountResult = m_Device.GetCycleCount();
        if (!cycleCountResult.has_value())
        {
            return std::unexpected(cycleCountResult.error());
        }

        const auto currentCycleBit = HasPersistenceToggleBit(*cycleCountResult);
        if (m_HasPersistedCycleBit && currentCycleBit == m_LastPersistedCycleBit)
        {
            return {};
        }

        const auto learningParametersResult = m_Device.GetAlgorithmLearningParameters();
        if (!learningParametersResult.has_value())
        {
            return std::unexpected(learningParametersResult.error());
        }

        const auto payload = SerializeLearningParameters(*learningParametersResult);
        if (auto saveResult = m_Store.Save(payload); !saveResult.has_value())
        {
            return std::unexpected(saveResult.error());
        }

        m_LastPersistedCycleBit = currentCycleBit;
        m_HasPersistedCycleBit = true;
        return {};
    }

    Result<void> Provider::RestorePersistedLearningParameters()
    {
        const auto loadResult = m_Store.Load();
        if (!loadResult.has_value())
        {
            if (loadResult.error().Condition == ErrorCondition::NotFound)
            {
                return {};
            }

            SPDLOG_LOGGER_WARN(
                m_Logger,
                "Ignoring persisted battery learning parameters because loading failed: {}",
                static_cast<int>(loadResult.error().Condition));
            return {};
        }

        const auto deserializedResult = DeserializeLearningParameters(*loadResult);
        if (!deserializedResult.has_value())
        {
            SPDLOG_LOGGER_WARN(m_Logger, "Ignoring persisted battery learning parameters because payload format is invalid");
            return {};
        }

        if (auto result = m_Device.SetAlgorithmLearningParameters(*deserializedResult); !result.has_value())
        {
            m_LastReadError = result.error();
            return std::unexpected(result.error());
        }

        return {};
    }
}
