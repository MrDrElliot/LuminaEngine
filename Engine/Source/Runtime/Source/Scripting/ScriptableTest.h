#pragma once

#include "Containers/Name.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Templates/Optional.h"
#include "Core/Threading/Thread.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/ComponentRef.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/Entity/Systems/ExclusiveSystem.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "ScriptableTest.generated.h"

namespace Lumina
{
    class CWorld;

    // Test type for the overridable-event pipeline, a C# subclass overriding OnTest and OnEchoWorld.
    REFLECT(Scriptable)
    class RUNTIME_API CScriptableTest : public CObject
    {
        GENERATED_BODY()
    public:

        // A native property, so a test proves one appended to a minted subclass coexists with it.
        PROPERTY(Hidden)
        float NativeValue = 1.5f;

        FUNCTION()
        virtual int32 OnTest(int32 X) { return X * 2; }

        // Object arg + return: the C++ default echoes the input; a C# override could return a different object.
        FUNCTION()
        virtual CWorld* OnEchoWorld(CWorld* In) { return In; }

        // A reflected optional, so the frame marshaller has a real TOptional slot to bind a C# nullable to.
        FUNCTION()
        virtual TOptional<float> OnEchoOptional(TOptional<float> In) { return In; }

        // A reflected container, so the frame marshaller has a real vector slot to bind a C# view to.
        FUNCTION()
        virtual void OnAppendSum(TVector<float>& Values) { Values.Add(0.0f); }

        // A native inspector button, so the generated function table has metadata to carry.
        FUNCTION(Button = "Reset Value", ToolTip = "Puts NativeValue back to its default")
        void ResetNativeValue() { NativeValue = 1.5f; }
    };

    /// Test C++ entity script, ticked by the same driver as a C# one, counting what ran so a test can assert the order.
    REFLECT()
    class RUNTIME_API CEntityScriptTest : public CEntityScript
    {
        GENERATED_BODY()
    public:

        void OnAttach() override      { ++AttachCount; Record('A'); }
        void OnReady() override       { ++ReadyCount; Record('R'); if (ReadyHook != nullptr) { ReadyHook(*this, HookContext); } }
        void OnUpdate(float Dt) override      { ++UpdateCount; AccumulatedTime += Dt; }
        void OnFixedUpdate(float Dt) override { ++FixedUpdateCount; }
        void OnInput(SInputEvent Event) override { Record('I'); }
        void OnDetach() override      { ++DetachCount; if (DetachHook != nullptr) { DetachHook(*this, HookContext); } }

        // Appends Phase and Label to a test's log, so a test can read back the order callbacks ran in. Unreflected.
        TVector<FString>* EventLog = nullptr;
        FString           Label;

        void Record(char Phase)
        {
            if (EventLog != nullptr)
            {
                EventLog->Add(FString(1, Phase) + ":" + Label);
            }
        }

        // Lets a test make OnReady or OnDetach mutate the registry. Unreflected, so a clone never carries them over.
        void (*ReadyHook)(CEntityScriptTest&, void*) = nullptr;
        void (*DetachHook)(CEntityScriptTest&, void*) = nullptr;
        void* HookContext = nullptr;

        // A container property, so a duplicate test proves the copy gets this member's address rather than the object base.
        PROPERTY(Hidden)
        TVector<FName> Values;

        PROPERTY(Hidden, Replicated)
        int32 NetValue = 0;

        int32 AttachCount = 0;
        int32 ReadyCount = 0;
        int32 UpdateCount = 0;
        int32 FixedUpdateCount = 0;
        int32 DetachCount = 0;
        float AccumulatedTime = 0.0f;
    };

    // One reference of each binding, recording what OnAttach already saw.
    REFLECT()
    class RUNTIME_API CComponentRefScriptTest : public CEntityScript
    {
        GENERATED_BODY()
    public:

        void OnAttach() override { bTransformAtAttach = Transform.Get() != nullptr; }

        PROPERTY(Hidden, Component)
        TComponentRef<STransformComponent> Transform;

        PROPERTY(Hidden, Component = Get)
        TComponentRef<SNameComponent> Name;

        bool bTransformAtAttach = false;
    };

    // Opted into the parallel pass, recording the thread that ran it so a test can tell the pass fanned out.
    REFLECT(ParallelUpdate)
    class RUNTIME_API CParallelEntityScriptTest : public CEntityScriptTest
    {
        GENERATED_BODY()
    public:

        void OnUpdate(float Dt) override
        {
            CEntityScriptTest::OnUpdate(Dt);
            LastThreadID = Threading::GetThreadID();
            if (UpdateHook != nullptr)
            {
                UpdateHook(*this, HookContext);
            }
        }

        void OnFixedUpdate(float Dt) override
        {
            CEntityScriptTest::OnFixedUpdate(Dt);
            LastThreadID = Threading::GetThreadID();
        }

        // Lets a test break the promise from inside the parallel pass. Unreflected, so a clone never carries it over.
        void (*UpdateHook)(CParallelEntityScriptTest&, void*) = nullptr;

        uint64 LastThreadID = 0;
    };

    // Throwaway owner of a strong reference, so a test can assign out of the object being released.
    REFLECT()
    class RUNTIME_API CObjectRefTest : public CObject
    {
        GENERATED_BODY()
    public:

        TStrongObjectPtr<CObject> Child;
    };

    // Throwaway counter of its own teardown, so a test can prove OnDestroy is not reentered.
    REFLECT()
    class RUNTIME_API CDestroyCountTest : public CObject
    {
        GENERATED_BODY()
    public:

        void OnDestroy() override
        {
            ++DestroyCount;
            if (bReenterOnDestroy)
            {
                ConditionalBeginDestroy();
            }
        }

        // Defined in the .cpp so the runtime and the test binary share one count.
        static int32 DestroyCount;

        bool bReenterOnDestroy = false;
    };

    // Test world subsystem, gated off so discovery never puts one in a real world.
    REFLECT()
    class RUNTIME_API CWorldSubsystemTest : public CWorldSubsystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return bAllowCreation; }

        void OnInitialize() override  { ++InitializeCount; }
        void OnWorldReady() override  { ++ReadyCount; bReadySawInitialize = InitializeCount > 0; }
        void OnUpdate(float Dt) override { ++UpdateCount; AccumulatedTime += Dt; }
        void OnTeardown() override    { ++TeardownCount; }

        // Defined in the library, since an inline one would be a separate copy in each binary on Linux.
        static bool bAllowCreation;

        int32 InitializeCount = 0;
        int32 ReadyCount = 0;
        int32 UpdateCount = 0;
        int32 TeardownCount = 0;
        float AccumulatedTime = 0.0f;
        bool  bReadySawInitialize = false;
    };

    // Never created, so a test can prove a declining class is skipped in the same pass that creates another.
    REFLECT()
    class RUNTIME_API CWorldSubsystemDeclineTest : public CWorldSubsystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return false; }
    };

    struct FSystemParamTestEvent
    {
        int32 Value = 0;
    };

    struct FSystemParamTestState : ECS::FResource
    {
        int32 Value = 0;
    };

    struct FSystemParamTestMarker
    {
        int32 Value = 0;
    };

    // Holds one of each system member, so a test can see them declare themselves without any Configure call.
    REFLECT()
    class RUNTIME_API CSystemParamTest : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return false; }

        ECS::TQuery<ECS::Write<STransformComponent>, ECS::Read<SNameComponent>> Movers;
        ECS::TQuery<ECS::Read<STransformComponent>, ECS::Changed<STransformComponent>> Moved;
        ECS::TEventWriter<FSystemParamTestEvent>                                Sent;
        ECS::TEventReader<FSystemParamTestEvent>                                Received;
        TResMut<FSystemParamTestState>                                          State;
        ECS::TQuery<ECS::Read<SNameComponent>, ECS::With<FSystemParamTestMarker>> Marked;
    };

    // Throwaway entity system, gated off so discovery never puts one in a real world.
    REFLECT()
    class RUNTIME_API CEntitySystemTest : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        // Defined in the library, since an inline one would be a separate copy in each binary on Linux.
        static bool bAllowCreation;

        bool ShouldCreate() override { return bAllowCreation; }

        void Configure() override
        {
            ++ConfigureCount;
            RequireUpdate(EUpdateStage::PrePhysics);
            RequireUpdate(EUpdateStage::FrameEnd);
            DeclareWrite("STransformComponent");
            DeclareRead("SStaticMeshComponent");
        }

        void OnStartup(const SExclusiveContext& Context) override  { ++StartupCount; }
        void OnUpdate(const SSystemContext& Context) override   { ++UpdateCount; }
        void OnTeardown(const SExclusiveContext& Context) override { ++TeardownCount; }

        CWorld* GetOwningWorldForTest() const { return GetOwningWorld(); }

        int32 ConfigureCount = 0;
        int32 StartupCount = 0;
        int32 UpdateCount = 0;
        int32 TeardownCount = 0;
    };

    // Exclusive by class, so a test can prove the driver runs it alone.
    REFLECT()
    class RUNTIME_API CEntitySystemExclusiveTest : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return CEntitySystemTest::bAllowCreation; }

        void Configure() override { RequireUpdate(EUpdateStage::FrameStart); }
    };

    // Counts its FixedUpdate ticks, so a test can prove the world steps at a fixed rate whatever the frame delta.
    REFLECT()
    class RUNTIME_API CEntitySystemFixedUpdateTest : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return CEntitySystemTest::bAllowCreation; }

        void Configure() override { RequireUpdate(EUpdateStage::FixedUpdate); }

        void OnUpdate(const SSystemContext& Context) override
        {
            ++TickCount;
            LastStage     = Context.GetUpdateStage();
            LastDeltaTime = Context.GetDeltaTime();
        }

        int32        TickCount = 0;
        EUpdateStage LastStage = EUpdateStage::Max;
        float        LastDeltaTime = 0.0f;
    };

    // Only reads what CEntitySystemTest reads, so the two never conflict and may share a batch.
    REFLECT()
    class RUNTIME_API CEntitySystemReaderTest : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return CEntitySystemTest::bAllowCreation; }

        void Configure() override
        {
            RequireUpdate(EUpdateStage::PrePhysics);
            DeclareRead("SStaticMeshComponent");
        }
    };

    // Reads what CEntitySystemTest writes and declares no order, so only the data conflict sequences the two.
    REFLECT()
    class RUNTIME_API CEntitySystemTransformReaderTest : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return CEntitySystemTest::bAllowCreation; }

        void Configure() override
        {
            RequireUpdate(EUpdateStage::PrePhysics);
            DeclareRead("STransformComponent");
        }
    };

    // Shares only reads with CEntitySystemReaderTest, so nothing but its declared order keeps it after that system.
    REFLECT()
    class RUNTIME_API CEntitySystemAfterTest : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return CEntitySystemTest::bAllowCreation; }

        void Configure() override
        {
            RequireUpdate(EUpdateStage::PrePhysics);
            RunsAfter<CEntitySystemReaderTest>();
            // By name, the way a script or a module that cannot see the class declares it.
            RunsBeforeSystem("CEntitySystemLastTest");
            DeclareRead("SStaticMeshComponent");
        }
    };

    // Ordered after CEntitySystemAfterTest from the other side, with a RunsBefore on its predecessor.
    REFLECT()
    class RUNTIME_API CEntitySystemLastTest : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return CEntitySystemTest::bAllowCreation; }

        void Configure() override
        {
            RequireUpdate(EUpdateStage::PrePhysics);
            DeclareRead("SStaticMeshComponent");
        }
    };

    // Closes a loop through the other ordered test systems, so a test can prove a cycle still schedules every system.
    REFLECT()
    class RUNTIME_API CEntitySystemCycleTest : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return CEntitySystemTest::bAllowCreation; }

        void Configure() override
        {
            RequireUpdate(EUpdateStage::PrePhysics);
            RunsAfter<CEntitySystemLastTest>();
            RunsBefore<CEntitySystemReaderTest>();
            DeclareRead("SStaticMeshComponent");
        }
    };

    // Records what its own constructor could see, so a test can prove identity lands before the body runs.
    REFLECT()
    class RUNTIME_API CConstructorIdentityTest : public CObject
    {
        GENERATED_BODY()
    public:

        CConstructorIdentityTest()
            : SeenClass(GetClass())
            , SeenName(GetName())
            , SeenPackage(GetPackage())
        {}

        CClass*   SeenClass;
        FName     SeenName;
        CPackage* SeenPackage;
    };

    // Asks for its own default object from its constructor, which without a guard recurses forever.
    REFLECT()
    class RUNTIME_API CSelfDefaultTest : public CObject
    {
        GENERATED_BODY()
    public:

        CSelfDefaultTest()
            : OwnDefault(bAskForOwnDefault ? StaticClass()->GetDefaultObject() : nullptr)
        {}

        // Off by default so startup does not log the guard; defined in the library so tests share its copy.
        static bool bAskForOwnDefault;

        CObject* OwnDefault;
    };
}
