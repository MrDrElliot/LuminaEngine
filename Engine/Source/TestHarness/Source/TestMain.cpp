#include "gtest/gtest.h"
#include "Containers/Name.h"
#include "Containers/Optional.h"
#include "Core/Application/ApplicationGlobalState.h"
#include "Core/Object/ObjectBase.h"
#include "Memory/Memory.h"
#include "TaskSystem/TaskSystem.h"

class EngineTestEnvironment : public ::testing::Environment
{
public:
    void SetUp() override
    {
        Lumina::Memory::Initialize();

        GlobalState.Emplace();
        Lumina::Jobs::Initialize();

        Lumina::InitializeCObjectSystem();
    }

    void TearDown() override
    {
        Lumina::ShutdownCObjectSystem();
        Lumina::Jobs::Shutdown();
        GlobalState.Reset();
    }

private:
    Lumina::TOptional<Lumina::FApplicationGlobalState> GlobalState;
};



int main(int Argc, char** Argv)
{
    ::testing::InitGoogleTest(&Argc, Argv);

    ::testing::AddGlobalTestEnvironment(new EngineTestEnvironment());
    return RUN_ALL_TESTS();
}

// The global new/delete overrides come from Memory/GlobalAllocatorOverrides.cpp. The build tool auto-adds it to every image, this one included.
