#include <gtest/gtest.h>

#include "Containers/StringFormat.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Particles/ParticleCustomModules.h"
#include "Particles/ParticleEmitterStack.h"
#include "Particles/ParticleGridModules.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"

using namespace Lumina;

namespace
{
    template<typename T>
    T* AddGridModule(CParticleEmitterStack* Stack, TVector<TStrongObjectPtr<CParticleModule>>& Modules)
    {
        T* Module = NewObject<T>(Stack->GetPackage());
        Modules.push_back(Module);
        return Module;
    }

    CParticleModule_SimulationStage* AddStage(CParticleEmitterStack* Stack, int32 Iterations)
    {
        CParticleModule_SimulationStage* Stage = AddGridModule<CParticleModule_SimulationStage>(Stack, Stack->GridModules);
        Stage->Iterations = Iterations;
        return Stage;
    }

    CParticleEmitterStack* MakeStack(const char* PackagePath)
    {
        CPackage* Package = CPackage::CreatePackage(PackagePath);
        return NewObject<CParticleEmitterStack>(Package, "Stack");
    }

    bool Contains(const FString& Source, const char* Text)
    {
        return Source.find(Text) != FString::npos;
    }

    // The real template lives under the engine's resource directory, which tests do not mount.
    FString Expand(const FParticleCompiler& Compiler)
    {
        return Compiler.ExpandTemplate("PARTICLE_GRID_CHANNELS = $PARTICLE_GRID_CHANNELS;\n$PARTICLE_GRID_CONSTANTS\n$PARTICLE_GRID_STAGE_FUNCS\n"
                                       "$PARTICLE_GRID_STAGE_CASES\n$PARTICLE_GRID_OUTPUT_FUNC\n$PARTICLE_UPDATE_FUNC\n");
    }
}

// Each Simulation Stage starts a function of its own, and a disabled one takes the modules under it out with it.
TEST(ParticleGridCompile, StagesSplitTheGridStack)
{
    CParticleEmitterStack* Stack = MakeStack("/ParticleGridCompile/Stages");

    AddStage(Stack, 1);
    AddGridModule<CParticleModule_GridBrush>(Stack, Stack->GridModules);
    AddStage(Stack, 20);
    AddGridModule<CParticleModule_FluidPressure>(Stack, Stack->GridModules);
    AddStage(Stack, 3)->bEnabled = false;
    AddGridModule<CParticleModule_GridDecay>(Stack, Stack->GridModules)->Channel = FName("Skipped");
    AddGridModule<CParticleModule_TargetGradient>(Stack, Stack->TargetModules);

    FParticleCompiler Compiler;
    Stack->CompileStacks(Compiler);
    ASSERT_FALSE(Compiler.HasErrors());

    EXPECT_TRUE(Compiler.GetGridStageIterations() == (TVector<int32>{ 1, 20 }));
    EXPECT_TRUE(Compiler.GetGridChannelNames() == (TVector<FString>{ "Density", "Pressure", "Divergence" }));

    const FString Source = Expand(Compiler);
    EXPECT_TRUE(Contains(Source, "PARTICLE_GRID_CHANNELS = 3u;"));
    EXPECT_TRUE(Contains(Source, "static const uint GRID_Pressure = 1u;"));
    EXPECT_TRUE(Contains(Source, "case 1u: GridStage1("));
    EXPECT_FALSE(Contains(Source, "GridStage2("));
    EXPECT_FALSE(Contains(Source, "GRID_Skipped"));
    EXPECT_TRUE(Contains(Source, "Color = SampleGradientLUT("));
    EXPECT_FALSE(Contains(Source, "$PARTICLE_GRID"));
}

// Grid modules ahead of the first Simulation Stage still run, once a frame, as a stage of their own.
TEST(ParticleGridCompile, ModulesBeforeAnyStageFormTheirOwn)
{
    CParticleEmitterStack* Stack = MakeStack("/ParticleGridCompile/Implicit");
    AddGridModule<CParticleModule_GridDecay>(Stack, Stack->GridModules);

    FParticleCompiler Compiler;
    Stack->CompileStacks(Compiler);

    EXPECT_TRUE(Compiler.GetGridStageIterations() == (TVector<int32>{ 1 }));
    EXPECT_EQ(Compiler.GetGridChannelCount(), 1u);
}

// A particle module that only deposits still gives the emitter a grid, with no stage for the renderer to run.
TEST(ParticleGridCompile, ADepositAloneDeclaresTheGrid)
{
    CParticleEmitterStack* Stack = MakeStack("/ParticleGridCompile/Deposit");
    AddGridModule<CParticleModule_WriteToGrid>(Stack, Stack->UpdateModules);

    FParticleCompiler Compiler;
    Stack->CompileStacks(Compiler);

    EXPECT_EQ(Compiler.GetGridChannelCount(), 1u);
    EXPECT_TRUE(Compiler.GetGridStageIterations().empty());
    EXPECT_TRUE(Contains(Expand(Compiler), "DepositGrid(GRID_Density, P.Position"));
}

// Iterations change how often the renderer dispatches, so retuning them takes the no-recompile path.
TEST(ParticleGridCompile, IterationsLeaveTheCodeAlone)
{
    CParticleEmitterStack* Stack = MakeStack("/ParticleGridCompile/Iterations");
    CParticleModule_SimulationStage* Stage = AddStage(Stack, 4);
    AddGridModule<CParticleModule_FluidPressure>(Stack, Stack->GridModules);

    FParticleCompiler Before;
    Stack->CompileStacks(Before);
    Stage->Iterations = 40;
    FParticleCompiler After;
    Stack->CompileStacks(After);

    EXPECT_EQ(Before.GetGeneratedCodeHash(), After.GetGeneratedCodeHash());
    EXPECT_TRUE(After.GetGridStageIterations() == (TVector<int32>{ 40 }));
}

// Channel names become Slang constants, so anything that is not an identifier is refused before it reaches the shader.
TEST(ParticleGridCompile, ChannelNamesMustBeIdentifiers)
{
    CParticleEmitterStack* Stack = MakeStack("/ParticleGridCompile/Names");
    AddGridModule<CParticleModule_GridBrush>(Stack, Stack->GridModules)->Channel = FName("two words");

    FParticleCompiler Compiler;
    Stack->CompileStacks(Compiler);
    EXPECT_TRUE(Compiler.HasErrors());
}

TEST(ParticleGridCompile, ACellHoldsAtMostSixteenChannels)
{
    CParticleEmitterStack* Stack = MakeStack("/ParticleGridCompile/Limit");
    CParticleModule_CustomGrid* Custom = AddGridModule<CParticleModule_CustomGrid>(Stack, Stack->GridModules);
    for (int32 Channel = 0; Channel <= kParticleGridMaxChannels; ++Channel)
    {
        Custom->GridChannels.push_back(FName(Format("Channel{}", Channel).c_str()));
    }

    FParticleCompiler Compiler;
    Stack->CompileStacks(Compiler);
    EXPECT_TRUE(Compiler.HasErrors());
    EXPECT_EQ(Compiler.GetGridChannelCount(), (uint32)kParticleGridMaxChannels);
}
