#pragma once

#include "Containers/Name.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Templates/Optional.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "ScriptableTest.generated.h"

namespace Lumina
{
    class CWorld;

    // Throwaway type proving the REFLECT(Scriptable) reflected-virtual pipeline end to end: a C# class can
    // subclass CScriptableTest and override these. The C++ defaults take over only when no managed override is
    // bound. OnTest exercises value (int) marshalling; OnEchoWorld exercises object (CObject pointer) marshalling
    // in BOTH directions (arg + return). Exercised by ScriptableTests.cpp and `script.scriptable_selftest`. Safe
    // to delete once the object-marshalling path has another test (GameInstance now consumes the arg direction).
    REFLECT(Scriptable)
    class RUNTIME_API CScriptableTest : public CObject
    {
        GENERATED_BODY()
    public:

        // A native reflected property, so the minted-property spike can prove that a property appended to a
        // minted subclass coexists with one declared in C++ (chaining, offsets, tagged serialization).
        PROPERTY()
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
        virtual void OnAppendSum(TVector<float>& Values) { Values.push_back(0.0f); }
    };

    /**
     * Throwaway C++ entity script, proving the Phase 5 unification from the native side: it subclasses the
     * SAME CEntityScript a C# script subclasses, and the driver ticks it through the same virtual calls with
     * no language-specific path. Counters record what ran so a test can assert the lifecycle order.
     */
    REFLECT()
    class RUNTIME_API CEntityScriptTest : public CEntityScript
    {
        GENERATED_BODY()
    public:

        void OnAttach() override      { ++AttachCount; }
        void OnReady() override       { ++ReadyCount; }
        void OnUpdate(float Dt) override      { ++UpdateCount; AccumulatedTime += Dt; }
        void OnFixedUpdate(float Dt) override { ++FixedUpdateCount; }
        void OnDetach() override      { ++DetachCount; if (DetachHook != nullptr) { DetachHook(*this, HookContext); } }

        // Lets a test make OnDetach mutate the registry. Unreflected, so a clone never carries it over.
        void (*DetachHook)(CEntityScriptTest&, void*) = nullptr;
        void* HookContext = nullptr;

        // A reflected CONTAINER property, so a clone/duplicate test can prove the copy is handed this
        // member's address rather than the object base (which would land the array header on the vtable ptr).
        PROPERTY()
        TVector<FName> Values;

        int32 AttachCount = 0;
        int32 ReadyCount = 0;
        int32 UpdateCount = 0;
        int32 FixedUpdateCount = 0;
        int32 DetachCount = 0;
        float AccumulatedTime = 0.0f;
    };

    /** Throwaway owner of a strong reference, so a test can assign out of the object being released. */
    REFLECT()
    class RUNTIME_API CObjectRefTest : public CObject
    {
        GENERATED_BODY()
    public:

        TObjectPtr<CObject> Child;
    };

    /** Throwaway counter of its own teardown, so a test can prove OnDestroy is not reentered. */
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

        // Static, since the count has to outlive the object it counts.
        static inline int32 DestroyCount = 0;

        bool bReenterOnDestroy = false;
    };

    // Throwaway world subsystem, gated off so discovery never puts one in a real world. The test flips the
    // gate to prove CreateMissing honors ShouldCreate and runs the lifecycle in order.
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

        static inline bool bAllowCreation = false;

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

    // Throwaway entity system, gated off so discovery never puts one in a real world.
    REFLECT()
    class RUNTIME_API CEntitySystemTest : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        static inline bool bAllowCreation = false;

        bool ShouldCreate() override { return bAllowCreation; }

        void Configure() override
        {
            ++ConfigureCount;
            RequireUpdate(EUpdateStage::PrePhysics, EUpdatePriority::High);
            RequireUpdate(EUpdateStage::FrameEnd);
            Writes<STransformComponent>();
            DeclareRead("SStaticMeshComponent");
        }

        void OnStartup() override  { ++StartupCount; }
        void OnUpdate() override   { ++UpdateCount; }
        void OnTeardown() override { ++TeardownCount; }

        int32 ConfigureCount = 0;
        int32 StartupCount = 0;
        int32 UpdateCount = 0;
        int32 TeardownCount = 0;
    };

    // Declares no access, so a test can prove the driver falls back to running it alone.
    REFLECT()
    class RUNTIME_API CEntitySystemExclusiveTest : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        bool ShouldCreate() override { return CEntitySystemTest::bAllowCreation; }

        void Configure() override { RequireUpdate(EUpdateStage::FrameStart); }
    };

    /** Records what its own constructor could see, so a test can prove identity lands before the body runs. */
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
}
