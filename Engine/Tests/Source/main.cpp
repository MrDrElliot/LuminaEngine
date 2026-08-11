#include "gtest/gtest.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectBase.h"
#include "EASTL/allocator.h"
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

    // Benchmarks (*Bench*) and the slow perf smoke tests (*Perf*) are excluded from the default run so
    // the correctness suite stays fast. Pass an explicit --gtest_filter to run them, e.g.
    //   Tests.exe --gtest_filter=TaskBench.*
    if (::testing::GTEST_FLAG(filter) == "*")
    {
        ::testing::GTEST_FLAG(filter) = "-*Bench*:*Perf*";
    }

    ::testing::AddGlobalTestEnvironment(new EngineTestEnvironment());
    return RUN_ALL_TESTS();
}

// The eastl::allocator binding is provided by the canonical Memory/EASTLImpl.cpp, and the global
// new/delete overrides by Memory/GlobalAllocatorOverrides.cpp. The build tool auto-adds both to
// every image, this one included. (Both were duplicated here before they were centralized.)