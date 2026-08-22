#include "gtest/gtest.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectBase.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "TaskSystem/TaskSystem.h"

class EngineTestEnvironment : public ::testing::Environment
{
public:
    void SetUp() override
    {
        Lumina::Memory::Initialize();

        Lumina::Threading::Initialize("Main Thread");
        Lumina::Logging::Init();
        Lumina::Task::Initialize();

        Lumina::InitializeCObjectSystem();
    }

    void TearDown() override
    {
        Lumina::Task::Shutdown();
        Lumina::Logging::Shutdown();
        Lumina::Threading::Shutdown();
        Lumina::ShutdownCObjectSystem();

    }
};



int main(int Argc, char** Argv)
{
    ::testing::InitGoogleTest(&Argc, Argv);

    // Benchmarks and slow perf tests are excluded by default; pass an explicit --gtest_filter.
    if (::testing::GTEST_FLAG(filter) == "*")
    {
        ::testing::GTEST_FLAG(filter) = "-*Bench*:*Perf*";
    }

    ::testing::AddGlobalTestEnvironment(new EngineTestEnvironment());
    return RUN_ALL_TESTS();
}

// The global new/delete overrides come from Memory/GlobalAllocatorOverrides.cpp. The build tool auto-adds it to every image, this one included.
