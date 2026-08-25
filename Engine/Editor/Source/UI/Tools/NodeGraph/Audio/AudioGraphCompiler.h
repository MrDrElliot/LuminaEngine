#pragma once

#include "Audio/Graph/AudioGraphProgram.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"

namespace Lumina
{
    class CAudioNodeGraph;
    class CAudioStream;

    struct FAudioGraphCompileResult
    {
        bool bSuccess = false;

        FAudioGraphProgram Program;

        /** Wave assets the program addresses by index. */
        TVector<TObjectPtr<CAudioStream>> Waves;

        TVector<FString> Errors;
        TVector<FString> Warnings;
    };

    /** Flattens an editor audio graph into the program the mixer runs. */
    class FAudioGraphCompiler
    {
    public:

        static FAudioGraphCompileResult Compile(CAudioNodeGraph* Graph);
    };
}
