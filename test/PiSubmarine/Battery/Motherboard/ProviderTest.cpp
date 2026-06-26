#include <gtest/gtest.h>

#include "PiSubmarine/Battery/Motherboard/Provider.h"
#include "PiSubmarine/Battery/Persistence/Api/IStoreMock.h"
#include "PiSubmarine/Chipset/Api/IClient.h"
#include "PiSubmarine/Error/Api/MakeError.h"
#include "PiSubmarine/I2C/Api/IDriver.h"
#include "PiSubmarine/Logging/Api/IFactory.h"
#include "PiSubmarine/Max17261/Device.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <string>
#include <vector>

namespace PiSubmarine::Battery::Motherboard
{
    namespace
    {
        constexpr std::uint32_t PersistencePayloadVersion = 1;

        class RegisterBackedDriver final : public I2C::Api::IDriver
        {
        public:
            std::array<std::uint16_t, 256> Registers{};

            [[nodiscard]] Error::Api::Result<void> Write(
                const std::uint8_t,
                const std::span<const std::uint8_t> txData) override
            {
                if (txData.size() != 3)
                {
                    return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
                }

                const auto offset = txData[0];
                Registers[offset] = static_cast<std::uint16_t>(txData[1])
                    | (static_cast<std::uint16_t>(txData[2]) << 8);
                return {};
            }

            [[nodiscard]] Error::Api::Result<void> Read(const std::uint8_t, const std::span<std::uint8_t>) override
            {
                return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
            }

            [[nodiscard]] Error::Api::Result<void> WriteRead(
                const std::uint8_t,
                const std::span<const std::uint8_t> txData,
                const std::span<std::uint8_t> rxData) override
            {
                if (txData.size() != 1 || rxData.size() != 2)
                {
                    return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
                }

                const auto raw = Registers[txData[0]];
                rxData[0] = static_cast<std::uint8_t>(raw & 0xFFu);
                rxData[1] = static_cast<std::uint8_t>((raw >> 8) & 0xFFu);
                return {};
            }
        };

        class ChipsetClientStub final : public Chipset::IClient
        {
        public:
            [[nodiscard]] Error::Api::Result<Chipset::Api::Status> GetStatus() const override
            {
                return Chipset::Api::Status{0};
            }

            [[nodiscard]] Error::Api::Result<std::chrono::seconds> GetRtc() override
            {
                return std::chrono::seconds{0};
            }

            [[nodiscard]] Error::Api::Result<void> SetRtc(const std::chrono::seconds&) override
            {
                return {};
            }

            [[nodiscard]] Error::Api::Result<Volts> GetChipsetVoltage() override
            {
                return Volts{0.0};
            }

            [[nodiscard]] Error::Api::Result<Volts> GetRegulator5Voltage() override
            {
                return Volts{0.0};
            }

            [[nodiscard]] Error::Api::Result<Volts> GetRegulatorPiVoltage() override
            {
                return Volts{0.0};
            }

            [[nodiscard]] Error::Api::Result<NormalizedFraction> GetBallastPosition() override
            {
                return NormalizedFraction{0.0};
            }

            [[nodiscard]] Error::Api::Result<Celsius> GetChipsetTemperature() override
            {
                return Celsius{0.0};
            }

            [[nodiscard]] Error::Api::Result<Celsius> GetChargerTemperature() override
            {
                return ChargerTemperature;
            }

            [[nodiscard]] Error::Api::Result<Amperes> GetChargerBusCurrent() override
            {
                return ChargerCurrent;
            }

            [[nodiscard]] Error::Api::Result<Amperes> GetBatteryCurrent() override
            {
                return Amperes{0.0};
            }

            [[nodiscard]] Error::Api::Result<Volts> GetChargerBusVoltage() override
            {
                return ChargerVoltage;
            }

            [[nodiscard]] Error::Api::Result<Volts> GetBatteryVoltage() override
            {
                return Volts{0.0};
            }

            [[nodiscard]] Error::Api::Result<Volts> GetChargerSystemVoltage() override
            {
                return Volts{0.0};
            }

            [[nodiscard]] Error::Api::Result<Celsius> GetBatteryTemperature() override
            {
                return BatteryTemperature;
            }

            [[nodiscard]] Error::Api::Result<void> SendCommand(Chipset::Api::Command) override
            {
                return {};
            }

            Volts ChargerVoltage{16.8};
            Amperes ChargerCurrent{1.2};
            Celsius ChargerTemperature{30.0};
            Celsius BatteryTemperature{28.5};
        };

        class LoggerFactoryStub final : public Logging::Api::IFactory
        {
        public:
            [[nodiscard]] std::shared_ptr<spdlog::logger> CreateLogger(std::string_view name) override
            {
                auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
                return std::make_shared<spdlog::logger>(std::string(name), sink);
            }
        };

        void WriteU32(std::vector<std::byte>& data, const std::uint32_t value)
        {
            data.push_back(static_cast<std::byte>(value & 0xFFu));
            data.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
            data.push_back(static_cast<std::byte>((value >> 16) & 0xFFu));
            data.push_back(static_cast<std::byte>((value >> 24) & 0xFFu));
        }

        [[nodiscard]] std::vector<std::byte> SerializePayload(const Max17261::AlgorithmLearningParameters& parameters)
        {
            const auto serialized = parameters.Serialize();
            std::vector<std::byte> payload;
            payload.reserve(sizeof(std::uint32_t) * 2 + serialized.size());
            WriteU32(payload, PersistencePayloadVersion);
            WriteU32(payload, static_cast<std::uint32_t>(serialized.size()));
            for (const auto value : serialized)
            {
                payload.push_back(static_cast<std::byte>(value));
            }
            return payload;
        }

        void SeedTelemetryRegisters(RegisterBackedDriver& driver)
        {
            driver.Registers[static_cast<std::uint8_t>(Max17261::RegOffset::Status)] = 0;
            driver.Registers[static_cast<std::uint8_t>(Max17261::RegOffset::VCell)] =
                Max17261::MicroVolts{15200000}.ToRaw();
            driver.Registers[static_cast<std::uint8_t>(Max17261::RegOffset::Current)] =
                static_cast<std::uint16_t>(Max17261::MicroAmperes{-1750000}.ToRaw());
            driver.Registers[static_cast<std::uint8_t>(Max17261::RegOffset::RepCap)] =
                Max17261::MicroAmpereHours{4200000}.ToRaw();
            driver.Registers[static_cast<std::uint8_t>(Max17261::RegOffset::RepSOC)] = 50u * 256u;
            driver.Registers[static_cast<std::uint8_t>(Max17261::RegOffset::TTF)] = 80;
            driver.Registers[static_cast<std::uint8_t>(Max17261::RegOffset::TTE)] = 100;
            driver.Registers[static_cast<std::uint8_t>(Max17261::RegOffset::DieTemp)] =
                static_cast<std::uint16_t>(MilliCelsius{31250}.ToRaw());
            driver.Registers[static_cast<std::uint8_t>(Max17261::RegOffset::Cycles)] = 0x003F;
        }
    }

    TEST(ProviderTest, InitializeRestoresPersistedParametersAndReturnsTelemetryState)
    {
        RegisterBackedDriver driver;
        SeedTelemetryRegisters(driver);

        Max17261::AlgorithmLearningParameters expectedParameters{};
        expectedParameters.LearningConfig.Raw = 0x1111;
        expectedParameters.FilterConfig.Raw = 0x2222;
        expectedParameters.RelaxConfig.Raw = 0x3333;
        expectedParameters.MiscConfig.Raw = 0x4444;
        expectedParameters.TempGain.Raw = 0x5555;
        expectedParameters.TempOffset.Raw = 0x6666;
        expectedParameters.ChargeCurrentGain.Raw = 0x7777;
        expectedParameters.ChargeCurrentOffset.Raw = 0x8888;
        expectedParameters.TemperatureCompensationBaselineRaw = 0x9999;
        expectedParameters.TemperatureCompensationCoefficientRaw = 0xAAAA;
        expectedParameters.CapacityDeltaAccumulator.Raw = 0xBBBB;
        expectedParameters.PowerDeltaAccumulator.Raw = 0xCCCC;
        expectedParameters.ReportedFullCapacity = Max17261::MicroAmpereHours::FromRaw(0x1234);
        expectedParameters.NominalFullCapacity = Max17261::MicroAmpereHours::FromRaw(0x2345);
        expectedParameters.CycleCount = 0x3456;

        Persistence::Api::IStoreMock store;
        EXPECT_CALL(store, Load())
            .WillOnce(testing::Return(Error::Api::Result<std::vector<std::byte>>(SerializePayload(expectedParameters))));

        ChipsetClientStub chipsetClient;
        LoggerFactoryStub loggerFactory;
        Max17261::Device device(driver);
        Provider provider(
            device,
            chipsetClient,
            store,
            loggerFactory,
            Config{
                .DesignCapacity = Max17261::MicroAmpereHours{5000000},
                .ChargeTerminationCurrent = Max17261::MicroAmperes{200000},
                .EmptyVoltage = Max17261::MicroVolts{12000000},
                .RefreshInterval = std::chrono::milliseconds{100},
                .WaitFunction = [](const std::chrono::milliseconds&) {},
                .ForceGaugeReset = false});

        const auto initializeResult = provider.Initialize();
        ASSERT_TRUE(initializeResult.has_value());

        const auto restoredParameters = device.GetAlgorithmLearningParameters();
        ASSERT_TRUE(restoredParameters.has_value());
        EXPECT_EQ(restoredParameters->LearningConfig.Raw, expectedParameters.LearningConfig.Raw);
        EXPECT_EQ(restoredParameters->ReportedFullCapacity.ToRaw(), expectedParameters.ReportedFullCapacity.ToRaw());
        EXPECT_EQ(restoredParameters->NominalFullCapacity.ToRaw(), expectedParameters.NominalFullCapacity.ToRaw());
        EXPECT_EQ(restoredParameters->CycleCount, expectedParameters.CycleCount);

        const auto stateResult = provider.GetState();
        ASSERT_TRUE(stateResult.has_value());
        EXPECT_DOUBLE_EQ(stateResult->PackVoltage.Value, 15.2);
        EXPECT_DOUBLE_EQ(stateResult->ChargerVoltage.Value, 16.8);
        EXPECT_DOUBLE_EQ(stateResult->PackCurrent.Value, -1.75);
        EXPECT_DOUBLE_EQ(stateResult->ChargerCurrent.Value, 1.2);
        EXPECT_DOUBLE_EQ(stateResult->ChargerTemperature.Value, 30.0);
        EXPECT_DOUBLE_EQ(stateResult->PackTemperature.Value, 28.5);
        EXPECT_DOUBLE_EQ(stateResult->MonitorTemperature.Value, 31.25);
        EXPECT_DOUBLE_EQ(stateResult->RemainingCapacity.Value, 4.2);
        EXPECT_DOUBLE_EQ(static_cast<double>(stateResult->StateOfCharge), 0.5);
        ASSERT_TRUE(stateResult->TimeToFull.has_value());
        ASSERT_TRUE(stateResult->TimeToEmpty.has_value());
        EXPECT_EQ(*stateResult->TimeToFull, std::chrono::milliseconds{450000});
        EXPECT_EQ(*stateResult->TimeToEmpty, std::chrono::milliseconds{562500});
    }

    TEST(ProviderTest, TickPersistsLearningParametersWhenCyclesBitSixToggles)
    {
        RegisterBackedDriver driver;
        SeedTelemetryRegisters(driver);

        Persistence::Api::IStoreMock store;
        EXPECT_CALL(store, Load())
            .WillOnce(testing::Return(std::unexpected(
                Error::Api::MakeError(Error::Api::ErrorCondition::NotFound))));
        EXPECT_CALL(store, Save(testing::_))
            .WillOnce(testing::Invoke([](const std::span<const std::byte> data)
            {
                EXPECT_EQ(data.size(), sizeof(std::uint32_t) * 2 + Max17261::AlgorithmLearningParameters::SerializedSize);
                return Error::Api::Result<void>{};
            }));

        ChipsetClientStub chipsetClient;
        LoggerFactoryStub loggerFactory;
        Max17261::Device device(driver);
        Provider provider(
            device,
            chipsetClient,
            store,
            loggerFactory,
            Config{
                .DesignCapacity = Max17261::MicroAmpereHours{5000000},
                .ChargeTerminationCurrent = Max17261::MicroAmperes{200000},
                .EmptyVoltage = Max17261::MicroVolts{12000000},
                .RefreshInterval = std::chrono::milliseconds{100},
                .WaitFunction = [](const std::chrono::milliseconds&) {},
                .ForceGaugeReset = false});

        const auto initializeResult = provider.Initialize();
        ASSERT_TRUE(initializeResult.has_value());

        driver.Registers[static_cast<std::uint8_t>(Max17261::RegOffset::Cycles)] = 0x0040;

        provider.Tick(std::chrono::milliseconds{100}, std::chrono::milliseconds{100});
    }
}
