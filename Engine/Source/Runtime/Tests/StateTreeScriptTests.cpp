#include <gtest/gtest.h>

#include "AI/StateTree/StateTreeTestTypes.h"
#include "Containers/Algorithm.h"
#include "Containers/Format.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/DotNet/DotNetStateTree.h"
#include "Scripting/ManagedTypeRegistry.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "World/ECS/CommandBus.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/StateTreeComponent.h"
#include "World/World.h"
#include "WorldTestAccess.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaStateTreeScriptTests
{
    using namespace Lumina;
    using namespace Lumina::StateTreeTest;

    constexpr double kFrame = 1.0 / 60.0;

    // More than one parallel chunk, so C# tasks run on workers.
    constexpr int32 kPerGroup = 200;

    constexpr const char* kGoEvent = "StateTreeScriptTest.Go";

    using FLoadSourceFn     = int32(*)(const char*, int32);
    using FComponentQueryFn = void(*)(void*, const char*, int32, const char*, int32, int32, int32*);
    using FComponentRunFn   = void(*)(void*, int32);

    // The test script, whose task ticks by TickStep and gains a field when bExtraField reshapes it.
    FString ProbeSource(int32 TickStep, bool bExtraField, bool bSleeperTicks = false)
    {
        FString Source = R"(
            using Lumina;
            using LuminaSharp;

            [Reflect]
            public partial struct ProbeTask : IExtends<SStateTreeTaskBase>, IStateTreeTask
            {
                [Property] public int Input;
                [Property] public int Output;
                [Property] public int TickCount;
                [Property] public int Gain = 10;
        )";
        Source += bExtraField ? "[Property] public int Extra;\n" : "";
        Source += R"(
                public ProbeTask() { }

                public EStateTreeRunStatus EnterState(FStateTreeExecutionContext Context)
                {
                    Output = Input * Gain;
                    return EStateTreeRunStatus.Running;
                }

                public EStateTreeRunStatus Tick(FStateTreeExecutionContext Context)
                {
                    TickCount += )";
        Source += Format("{}", TickStep);
        Source += R"(;
                    Output = Input * Gain + TickCount;
                    return EStateTreeRunStatus.Running;
                }
            }

            [Reflect]
            public partial struct ProbeCondition : IExtends<SStateTreeConditionBase>, IStateTreeCondition
            {
                [Property] public int Value;
                [Property] public int Threshold;

                public bool TestCondition(FStateTreeExecutionContext Context) => Value >= Threshold;
            }

            [Reflect]
            public partial struct ProbeEvaluator : IExtends<SStateTreeEvaluatorBase>, IStateTreeEvaluator
            {
                [Property] public int Counter;

                public void Tick(FStateTreeExecutionContext Context) => Counter++;
            }

            [Reflect]
            public partial struct ProbeEmplaceTask : IExtends<SStateTreeTaskBase>, IStateTreeTask
            {
                [Property] public int Entered;

                public EStateTreeRunStatus EnterState(FStateTreeExecutionContext Context)
                {
                    Context.GetWorld()!.GetOrEmplaceComponent<SLifetimeComponent>(Context.GetOwner()).Lifetime = 5f;
                    Entered = 1;
                    return EStateTreeRunStatus.Running;
                }
            }

            [Reflect]
            public partial struct ProbeSleeperTask : IExtends<SStateTreeTaskBase>, IStateTreeTask
            {
                [Property] public int TickCount;
        )";
        Source += bSleeperTicks ? "public EStateTreeRunStatus Tick(FStateTreeExecutionContext Context) { ++TickCount; return EStateTreeRunStatus.Running; }\n" : "";
        Source += "}\n";
        return Source;
    }

    class FStateTreeScriptTest : public ::testing::Test
    {
    public:

        static void SetUpTestSuite()
        {
            Paths::InitializePaths();
            DotNet::Initialize();
        }

        static void TearDownTestSuite()
        {
            if (DotNet::IsInitialized())
            {
                LoadScripts(FString());
                DotNet::Shutdown();
            }
        }

    protected:

        void SetUp() override
        {
            if (!DotNet::IsInitialized())
            {
                GTEST_SKIP() << "the managed host did not start in this process";
            }
            RegisterPendingTypes();
            ASSERT_TRUE(LoadScripts(ProbeSource(1, false))) << "the test script compiles";
            World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            World->GetEntityRegistry().Ctx().Emplace<CWorld*>(World);
            FWorldTestAccess::KeepOnlySystems(*World, { "CStateTreeSystem" });
        }

        // A reload as the host runs one, the script swap and then every native stage over what it published.
        static bool LoadScripts(const FString& Source)
        {
            auto* Load = (FLoadSourceFn)DotNet::ResolveManagedExport("Test_LoadScriptSource");
            if (Load == nullptr)
            {
                return false;
            }
            Scripting::RegisterBuiltInManagedTypeStages();
            Scripting::FManagedTypeRegistry::Get().PreUnloadAll();
            if (Load(*Source, Source.Len()) != 0)
            {
                Scripting::FManagedTypeRegistry::Get().UnloadAbortedAll();
                return false;
            }
            TVector<Scripting::FManagedTypeDefinition> Definitions;
            TVector<Scripting::FManagedTypeAlias> Aliases;
            DotNet::GatherManagedTypeDefinitions(Definitions, Aliases);
            Scripting::FManagedTypeRegistry::Get().CompileAll(Definitions, Aliases);
            return true;
        }

        static CStruct* ScriptType(const char* Name) { return ResolveStructByName(FName(Name)); }

        template<typename TBase>
        static SStateTreeNodeBase& AddScriptNode(TVector<TInstancedStruct<TBase>>& Slots, const char* Type, const char* Name)
        {
            TInstancedStruct<TBase>& Slot = Slots.AddDefaultedGetRef();
            Slot.InitializeAs(ScriptType(Type));
            SStateTreeNodeBase& Node = *Slot.template GetMutablePtr<SStateTreeNodeBase>();
            Node.Name = FName(Name);
            return Node;
        }

        static int32& Field(const CStruct* Type, void* Data, const char* Name)
        {
            return *Type->GetProperty(FName(Name))->GetValuePtr<int32>(Data);
        }

        // An authored field of a C# node, the value each instance starts from.
        template<typename TBase>
        static int32& AuthoredField(TInstancedStruct<TBase>& Slot, const char* Name)
        {
            return Field(Slot.GetScriptStruct(), Slot.GetMutableMemory(), Name);
        }

        // Wait holds a C# task and a native task bound to it, and leaves for Go once a C# condition on a C# evaluator passes.
        static CStateTree* MakeTree(int32 Threshold)
        {
            CStateTree* Tree = NewObject<CStateTree>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            Tree->Parameters.InitializeAs<SStateTreeTestParameters>().Speed = 3;
            AddScriptNode(Tree->Evaluators, "ProbeEvaluator", "Eval");

            SStateTreeState& Wait = Tree->States[AddState(*Tree, "Wait")];
            Bind(AddScriptNode(Wait.Tasks, "ProbeTask", "Script"), "Input", EStateTreeBindingSource::Parameter, "", "Speed");
            AuthoredField(Wait.Tasks.Last(), "Gain") = 7;
            Bind(AddNode<SStateTreeTestTask>(Wait.Tasks, "Native"), "Input", EStateTreeBindingSource::Task, "Script", "Output");

            SStateTreeTransition& ToGo = AddTransition(Wait, EStateTreeTransitionTrigger::OnTick, EStateTreeTransitionTarget::State, "Go");
            Bind(AddScriptNode(ToGo.Conditions, "ProbeCondition", "Ready"), "Value", EStateTreeBindingSource::Evaluator, "Eval", "Counter");
            AuthoredField(ToGo.Conditions.Last(), "Threshold") = Threshold;

            AddNode<SStateTreeTestTask>(Tree->States[AddState(*Tree, "Go")].Tasks, "After");
            EXPECT_TRUE(Tree->Compile());
            return Tree;
        }

        static int32 TaskField(const FStateTreeInstance& Instance, const FStateTreeRuntime& Runtime, int32 Task, const char* Name)
        {
            const uint16 Node = (uint16)(Runtime.States[FindState(Runtime, "Wait")].FirstTask + Task);
            return Field(Runtime.Nodes[Node].InstanceDataType.Get(), Instance.GetNodeInstanceData(Node), Name);
        }

        TVector<ECS::FEntity> Spawn(CStateTree* Tree, int32 Count) const
        {
            TVector<ECS::FEntity> Entities;
            for (int32 Index = 0; Index < Count; ++Index)
            {
                const ECS::FEntity Entity = World->GetEntityRegistry().Create();
                World->GetEntityRegistry().Emplace<SStateTreeComponent>(Entity).StateTree = Tree;
                Entities.Add(Entity);
            }
            return Entities;
        }

        SStateTreeComponent& Component(ECS::FEntity Entity) const { return World->GetEntityRegistry().Get<SStateTreeComponent>(Entity); }

        void Tick(double Time) const
        {
            const_cast<SSystemContext&>(FWorldTestAccess::SystemContext(*World)).Time = Time;
            FWorldTestAccess::TickStage(*World, EUpdateStage::PrePhysics, kFrame);
        }

        CWorld* World = nullptr;
    };

    TEST_F(FStateTreeScriptTest, CSharpNodesRunBesideNativeNodesWithBindingsBothWays)
    {
        for (const char* Name : { "ProbeTask", "ProbeEmplaceTask" })
        {
            EXPECT_NE(ResolveInstancedStructType(SStateTreeTaskBase::StaticStruct(), FName(Name)), nullptr) << Name << " is offered where a task goes";
        }
        EXPECT_NE(ResolveInstancedStructType(SStateTreeConditionBase::StaticStruct(), FName("ProbeCondition")), nullptr);
        EXPECT_NE(ResolveInstancedStructType(SStateTreeEvaluatorBase::StaticStruct(), FName("ProbeEvaluator")), nullptr);
        EXPECT_EQ(ResolveInstancedStructType(SStateTreeConditionBase::StaticStruct(), FName("ProbeTask")), nullptr);

        CStateTree* Tree = MakeTree(3);
        const TSharedPtr<const FStateTreeRuntime> Runtime = Tree->GetRuntime();
        ASSERT_TRUE(Runtime->IsValid());
        ASSERT_NE(Runtime->ScriptDispatch, nullptr);
        EXPECT_TRUE(StateTreeScript::NeedsTick(Runtime->ScriptDispatch, ScriptType("ProbeTask")));
        EXPECT_FALSE(StateTreeScript::NeedsTick(Runtime->ScriptDispatch, ScriptType("ProbeEmplaceTask"))) << "a C# task without a Tick lets its instance sleep";

        FStateTreeInstance Instance;
        ASSERT_EQ(Instance.Start(Runtime, nullptr, ECS::FEntity(5), 0.0), EStateTreeRunStatus::Running);
        EXPECT_EQ(TaskField(Instance, *Runtime, 0, "Output"), 21) << "the C# task read the bound parameter and its authored Gain";
        EXPECT_EQ(TaskField(Instance, *Runtime, 1, "Input"), 21) << "the native task read the C# task's output";

        Instance.Tick(0.1);
        EXPECT_EQ(TaskField(Instance, *Runtime, 0, "TickCount"), 1);
        EXPECT_EQ(TaskField(Instance, *Runtime, 1, "Input"), 22);
        EXPECT_EQ(Instance.GetActiveStateName(), FName("Wait")) << "the evaluator has counted two ticks, under the condition's threshold";

        Instance.Tick(0.2);
        EXPECT_EQ(Instance.GetActiveStateName(), FName("Go")) << "the C# condition passed once the C# evaluator bound into it reached three";
        EXPECT_EQ(Field(Runtime->Nodes[0].InstanceDataType.Get(), Instance.GetNodeInstanceData(0), "Counter"), 3);
        Instance.Stop(0.3);
    }

    TEST_F(FStateTreeScriptTest, AReloadKeepsTheTreeValidAndItsInstancesRunning)
    {
        CStateTree* Tree = MakeTree(1000);
        const TVector<ECS::FEntity> Agents = Spawn(Tree, 4);
        auto ScriptField = [&](const char* Name)
        {
            return TaskField(*Component(Agents[0]).Running.Instance, *Tree->GetRuntime(), 0, Name);
        };

        Tick(0.0);
        Tick(kFrame);
        const FStateTreeInstance* Started = Component(Agents[0]).Running.Instance.Get();
        ASSERT_EQ(ScriptField("TickCount"), 1);

        CStruct* Original = ScriptType("ProbeTask");
        const FStateTreeRuntime* OriginalRuntime = Tree->GetRuntime().Get();
        ASSERT_TRUE(LoadScripts(ProbeSource(100, false)));
        EXPECT_EQ(ScriptType("ProbeTask"), Original) << "a reload that only changes code keeps the minted struct";
        EXPECT_EQ(Tree->GetRuntime().Get(), OriginalRuntime) << "so the tree keeps its runtime";

        Tick(2.0 * kFrame);
        EXPECT_EQ(Component(Agents[0]).Running.Instance.Get(), Started);
        EXPECT_EQ(ScriptField("TickCount"), 101) << "the running instance carried on with the reloaded code";

        ASSERT_TRUE(LoadScripts(ProbeSource(100, true)));
        CStruct* Reshaped = ScriptType("ProbeTask");
        ASSERT_NE(Reshaped, Original) << "adding a field mints a new struct";
        EXPECT_NE(Tree->GetRuntime().Get(), OriginalRuntime) << "the reload stage recompiled the tree onto it";
        EXPECT_TRUE(Tree->GetCompileErrors().IsEmpty());
        EXPECT_EQ(Tree->States[0].Tasks[0].GetScriptStruct(), Reshaped);
        EXPECT_EQ(AuthoredField(Tree->States[0].Tasks[0], "Gain"), 7) << "the authored value migrated by name";

        Tick(3.0 * kFrame);
        for (ECS::FEntity Agent : Agents)
        {
            ASSERT_TRUE(Component(Agent).IsRunning()) << "the system restarted the instance on the recompiled tree";
            EXPECT_FALSE(Component(Agent).Running.Instance->IsSuperseded());
        }
        EXPECT_EQ(ScriptField("Output"), 21) << "the restarted C# task entered with its migrated Gain";
        const FStateTreeRuntime& Recompiled = *Tree->GetRuntime();
        EXPECT_NE(Recompiled.Nodes[Recompiled.States[0].FirstTask].InstanceDataType->GetProperty(FName("Extra")), nullptr);

        Tick(4.0 * kFrame);
        EXPECT_EQ(ScriptField("TickCount"), 100);
    }

    TEST_F(FStateTreeScriptTest, AReloadThatAddsATickWakesTheStateItLetSleep)
    {
        CStateTree* Tree = NewObject<CStateTree>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        AddScriptNode(Tree->States[AddState(*Tree, "Sleep")].Tasks, "ProbeSleeperTask", "Sleeper");
        ASSERT_TRUE(Tree->Compile());
        const TSharedPtr<const FStateTreeRuntime> SleepingRuntime = Tree->GetRuntime();
        ASSERT_FALSE(SleepingRuntime->States[0].bNeedsTick);

        ASSERT_TRUE(LoadScripts(ProbeSource(1, false, true)));
        EXPECT_EQ(ScriptType("ProbeSleeperTask"), Tree->States[0].Tasks[0].GetScriptStruct()) << "adding a method keeps the minted struct";
        EXPECT_NE(Tree->GetRuntime(), SleepingRuntime) <<"the reload stage recompiled the tree";
        EXPECT_TRUE(Tree->GetRuntime()->States[0].bNeedsTick);
    }

    TEST_F(FStateTreeScriptTest, AReloadThatRemovesANodeStopsTheInstancesRunningIt)
    {
        CStateTree* Tree = MakeTree(1000);
        const TVector<ECS::FEntity> Agents = Spawn(Tree, 4);
        Tick(0.0);
        ASSERT_TRUE(Component(Agents[0]).IsRunning());

        ASSERT_TRUE(LoadScripts("public static class ProbeLeftovers { }"));
        EXPECT_FALSE(Tree->GetCompileErrors().IsEmpty()) << "the recompiled tree reports the nodes it lost";
        Tick(kFrame);
        EXPECT_FALSE(Component(Agents[0]).IsRunning()) << "the instance exited its states past the task the reload removed";
    }

    TEST_F(FStateTreeScriptTest, CSharpDrivesTheComponentAndDefersItsChangesToTheCommandBus)
    {
        CStateTree* Tree = NewObject<CStateTree>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        Tree->Parameters.InitializeAs<SStateTreeTestParameters>().Speed = 3;
        SStateTreeState& Wait = Tree->States[AddState(*Tree, "Wait")];
        AddScriptNode(Wait.Tasks, "ProbeEmplaceTask", "Emplace");
        Bind(AddNode<SStateTreeTestTask>(Wait.Tasks, "Native"), "Input", EStateTreeBindingSource::Parameter, "", "Speed");
        SHierarchicalTag GoTag;
        GoTag.TagName = FName(kGoEvent);
        AddTransition(Wait, EStateTreeTransitionTrigger::OnEvent, EStateTreeTransitionTarget::State, "Go").EventTag = GoTag;
        AddNode<SStateTreeTestTask>(Tree->States[AddState(*Tree, "Go")].Tasks, "After");
        ASSERT_TRUE(Tree->Compile());
        const TVector<ECS::FEntity> Agents = Spawn(Tree, kPerGroup);

        int32 ImmediateEmplaces = 0;
        ECS::FSignalConnection Connection = World->OnConstruct<SLifetimeComponent>().Connect([](void* Payload, ECS::FRegistry&, ECS::FEntity)
        {
            *static_cast<int32*>(Payload) += ECS::FCommandBus::ShouldDefer() ? 1 : 0;
        }, &ImmediateEmplaces);
        Tick(0.0);
        Connection.Release();

        ECS::FRegistry& Registry = World->GetEntityRegistry();
        EXPECT_EQ(ImmediateEmplaces, 0) << "every component a C# task added waited for the command bus flush";
        EXPECT_TRUE(Algo::AllOf(Agents, [&](ECS::FEntity Agent)
        {
            const SLifetimeComponent* Lifetime = Registry.TryGet<SLifetimeComponent>(Agent);
            return Lifetime != nullptr && Lifetime->Lifetime == 5.0f;
        })) << "the flush applied what every C# task wrote";

        auto* Query = (FComponentQueryFn)DotNet::ResolveManagedExport("Test_StateTreeComponentQuery");
        auto* Run = (FComponentRunFn)DotNet::ResolveManagedExport("Test_StateTreeComponentRun");
        ASSERT_NE(Query, nullptr);
        ASSERT_NE(Run, nullptr);
        SStateTreeComponent& Driven = Component(Agents[0]);
        auto Ask = [&](const char* State, const char* Event, int32 NewSpeed)
        {
            TArray<int32, 4> Out = {};
            Query(&Driven, State, (int32)strlen(State), Event, (int32)strlen(Event), NewSpeed, Out.GetData());
            return Out;
        };

        const TArray<int32, 4> Before = Ask("Wait", kGoEvent, 9);
        EXPECT_EQ(Before[0], 1) << "IsRunning";
        EXPECT_EQ(Before[1], 1) << "GetActiveState";
        EXPECT_EQ(Before[2], 3) << "GetParameters read the live parameters";
        EXPECT_EQ(Before[3], 1) << "SendEvent queued";

        Tick(kFrame);
        EXPECT_EQ(TaskField(*Driven.Running.Instance, *Tree->GetRuntime(), 1, "Input"), 9) << "the parameter C# set reached the binding on the next tick";
        EXPECT_EQ(Ask("Go", "", 9)[1], 1) << "the event C# sent took the transition";
        EXPECT_EQ(Driven.GetActiveState(), FName("Go"));

        Run(&Driven, 0);
        Tick(2.0 * kFrame);
        EXPECT_FALSE(Driven.IsRunning()) << "Stop ended the tree";
        Tick(3.0 * kFrame);
        EXPECT_FALSE(Driven.IsRunning()) << "and it stays stopped";
        EXPECT_EQ(Ask("Wait", "", 9)[2], -1) << "a stopped tree has no live parameters";

        Run(&Driven, 1);
        Tick(4.0 * kFrame);
        EXPECT_TRUE(Driven.IsRunning()) << "Start ran it again from the root";
        EXPECT_EQ(Driven.GetActiveState(), FName("Wait"));
        EXPECT_TRUE(Component(Agents[1]).IsRunning()) << "the others never stopped";
        EXPECT_EQ(Component(Agents[1]).GetActiveState(), FName("Wait"));
    }
}
