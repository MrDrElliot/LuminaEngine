#pragma once

#include "Containers/Array.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    /** Immutable snapshot of a live FScopedSlowTask, handed to the UI for rendering. */
    struct FSlowTaskProgress
    {
        uint64          ID = 0;
        FFixedString    Title;
        FFixedString    Message;
        float           Fraction = 0.0f;
    };

    /** RAII progress reporter; the editor renders a modal per live instance. */
    class RUNTIME_API FScopedSlowTask
    {
    public:

        explicit FScopedSlowTask(float InTotalWork, FStringView InTitle = "Working...", FStringView InDefaultMessage = {});
        ~FScopedSlowTask();

        FScopedSlowTask(const FScopedSlowTask&) = delete;
        FScopedSlowTask& operator=(const FScopedSlowTask&) = delete;

        /** Advance progress by ExpectedWork and optionally replace the displayed message. Thread-safe. */
        void EnterProgressFrame(float ExpectedWork = 1.0f, FStringView Message = {});

        /** Replace the displayed message without advancing progress. Thread-safe. */
        void UpdateMessage(FStringView Message);

    private:

        // Caller must hold the registry mutex.
        void Publish();

        uint64          ID = 0;
        float           TotalWork = 1.0f;
        float           CompletedWork = 0.0f;
        FFixedString    Title;
        FFixedString    CurrentMessage;
    };

    namespace SlowTasks
    {
        /** True if any FScopedSlowTask is currently alive. */
        RUNTIME_API bool HasActive();

        /** Thread-safe copy of every live task's progress, for UI rendering. */
        RUNTIME_API TVector<FSlowTaskProgress> GetSnapshot();
    }
}
