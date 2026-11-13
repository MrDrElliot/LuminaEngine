#include "MCPParticleTools.h"
#include "MCPToolTestUtil.h"

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Particles/ParticleOps.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPParticleToolsTestsInternal
{
    class MCPParticleToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPParticleToolsTest() : FRegisteredToolsTest("MCPParticleToolsTests", &RegisterParticleTools) {}

        void SetUp() override
        {
            RegisterPendingTypes();
            FRegisteredToolsTest::SetUp();

            // CreatePackage refuses a path already in memory, so each test gets its own.
            const char* TestName = testing::UnitTest::GetInstance()->current_test_info()->name();
            System = NewObject<CParticleSystem>(CPackage::CreatePackage(FStringView(Format("/MCPParticleToolsTests/{}", TestName))), TestName);
            System->AddEmitter();
            Guid = FString(*System->GetGUID().ToString());
        }

        FJsonValue Edit(const char* Tool, const FString& Fields)
        {
            return ExpectSucceeded(FStringView(Tool), FStringView(Format(R"({{"System":"{}",{}}})", Guid, Fields)));
        }

        void Refuse(const char* Tool, const FString& Fields, const char* Expected)
        {
            ExpectRefused(FStringView(Tool), FStringView(Format(R"({{"System":"{}",{}}})", Guid, Fields)), Expected);
        }

        int32 AddEmptyEmitter()
        {
            return Edit("particle.emitter", R"("Action":"Add","Name":"Sparks","bStarterStack":false)")["structuredContent"]["Index"].AsInt32();
        }

        TVector<TStrongObjectPtr<CParticleModule>>& SpawnModules(int32 Emitter)
        {
            return *ParticleOps::GetModules(System, Emitter, EParticleModuleStage::Spawn);
        }

        FString SpawnModuleType(int32 Emitter, int32 Index)
        {
            return FString(*SpawnModules(Emitter)[Index]->GetClass()->GetName().ToString());
        }

        CParticleSystem* System = nullptr;
        FString Guid;
    };
}

using namespace MCPParticleToolsTestsInternal;

TEST_F(MCPParticleToolsTest, EmitterAddAndRemoveKeepAtLeastOne)
{
    EXPECT_EQ(AddEmptyEmitter(), 1);
    ASSERT_EQ(System->Emitters.Num(), 2);
    EXPECT_EQ(System->Emitters[1]->EmitterName, "Sparks");
    EXPECT_TRUE(SpawnModules(1).IsEmpty());

    Edit("particle.emitter", R"("Action":"Remove","Emitter":0)");
    ASSERT_EQ(System->Emitters.Num(), 1);
    EXPECT_EQ(System->Emitters[0]->EmitterName, "Sparks");

    Refuse("particle.emitter", R"("Action":"Remove","Emitter":3)", "Emitter 3 does not exist");
    Refuse("particle.emitter", R"("Action":"Remove","Emitter":0)", "only emitter");
    EXPECT_EQ(System->Emitters.Num(), 1);
}

TEST_F(MCPParticleToolsTest, ModuleAddMoveAndRemoveEditTheStageStack)
{
    const int32 Emitter = AddEmptyEmitter();

    const FJsonValue Lifetime = Edit("particle.module", Format(R"("Action":"Add","Emitter":{},"ModuleType":"Lifetime")", Emitter));
    EXPECT_EQ(FString(Lifetime["structuredContent"]["Stage"].AsString()), "Spawn");
    EXPECT_EQ(Lifetime["structuredContent"]["Index"].AsInt32(), 0);
    const FJsonValue Color = Edit("particle.module", Format(R"("Action":"Add","Emitter":{},"ModuleType":"InitialColor")", Emitter));
    EXPECT_EQ(Color["structuredContent"]["Index"].AsInt32(), 1);
    Refuse("particle.module", Format(R"("Action":"Add","Emitter":{},"ModuleType":"NoSuchModule")", Emitter), "No particle module type");

    Edit("particle.module", Format(R"("Action":"Move","Emitter":{},"Stage":"Spawn","Index":1,"NewIndex":0)", Emitter));
    ASSERT_EQ(SpawnModules(Emitter).Num(), 2);
    EXPECT_EQ(SpawnModuleType(Emitter, 0), "CParticleModule_InitialColor");
    EXPECT_EQ(SpawnModuleType(Emitter, 1), "CParticleModule_Lifetime");

    Refuse("particle.module", Format(R"("Action":"Move","Emitter":{},"Stage":"Spawn","Index":0,"NewIndex":2)", Emitter), "2 is not a position");
    Refuse("particle.module", Format(R"("Action":"Move","Emitter":{},"Stage":"Spawn","Index":0,"NewIndex":-1)", Emitter), "-1 is not a position");
    EXPECT_EQ(SpawnModuleType(Emitter, 0), "CParticleModule_InitialColor");

    Edit("particle.module", Format(R"("Action":"Remove","Emitter":{},"Stage":"Spawn","Index":0)", Emitter));
    ASSERT_EQ(SpawnModules(Emitter).Num(), 1);
    EXPECT_EQ(SpawnModuleType(Emitter, 0), "CParticleModule_Lifetime");
    Refuse("particle.module", Format(R"("Action":"Remove","Emitter":{},"Stage":"Spawn","Index":1)", Emitter), "There is no Spawn module 1");
}

TEST_F(MCPParticleToolsTest, UserParameterSetReplacesRejectsMismatchesAndRemoves)
{
    Edit("particle.user_parameter", R"("Name":"Speed","Type":"Float","Value":2.5)");
    Edit("particle.user_parameter", R"("Name":"Speed","Type":"Float","Value":4)");
    ASSERT_EQ(System->UserParameters.Num(), 1);
    EXPECT_FLOAT_EQ(System->UserParameters[0].Scalar, 4.0f);

    Refuse("particle.user_parameter", R"("Name":"Speed","Type":"Float","Value":true)", "A Float parameter takes a number");
    Refuse("particle.user_parameter", R"("Name":"Speed","Type":"Bool","Value":1)", "A Bool parameter takes true or false");
    Refuse("particle.user_parameter", R"("Name":"Speed","Type":"Color","Value":{"X":"red"})", "takes an object with X, Y, Z and W");
    Refuse("particle.user_parameter", R"("Name":"Speed","Type":"Quat","Value":1)", "'Quat' is not a parameter type");
    ASSERT_EQ(System->UserParameters.Num(), 1);
    EXPECT_EQ(System->UserParameters[0].Type, EParticleParameterType::Float);
    EXPECT_FLOAT_EQ(System->UserParameters[0].Scalar, 4.0f);

    Edit("particle.user_parameter", R"("Action":"Remove","Name":"Speed")");
    EXPECT_TRUE(System->UserParameters.IsEmpty());
    Refuse("particle.user_parameter", R"("Action":"Remove","Name":"Speed")", "There is no parameter named Speed");
}
