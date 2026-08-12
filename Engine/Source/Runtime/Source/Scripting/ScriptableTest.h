#pragma once

#include "Core/Object/Object.h"
#include "Scripting/EntityScript.h"
#include "ScriptableTest.generated.h"

namespace Lumina
{
    class CWorld;

    // Throwaway type proving the REFLECT(Scriptable) + FUNCTION(ScriptEvent) pipeline end to end: a C# class can
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

        FUNCTION(ScriptEvent)
        virtual int32 OnTest(int32 X) { return X * 2; }

        // Object arg + return: the C++ default echoes the input; a C# override could return a different object.
        FUNCTION(ScriptEvent)
        virtual CWorld* OnEchoWorld(CWorld* In) { return In; }
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
        void OnDetach() override      { ++DetachCount; }

        int32 AttachCount = 0;
        int32 ReadyCount = 0;
        int32 UpdateCount = 0;
        int32 FixedUpdateCount = 0;
        int32 DetachCount = 0;
        float AccumulatedTime = 0.0f;
    };
}
