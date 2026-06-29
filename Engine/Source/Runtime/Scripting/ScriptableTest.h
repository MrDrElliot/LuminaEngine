#pragma once

#include "Core/Object/Object.h"
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

        FUNCTION(ScriptEvent)
        virtual int32 OnTest(int32 X) { return X * 2; }

        // Object arg + return: the C++ default echoes the input; a C# override could return a different object.
        FUNCTION(ScriptEvent)
        virtual CWorld* OnEchoWorld(CWorld* In) { return In; }
    };
}
