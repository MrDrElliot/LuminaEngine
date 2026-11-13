#pragma once

#include "StateTreeCompiler.h"
#include "Containers/RingBuffer.h"
#include "Containers/Span.h"
#include "Containers/StaticArray.h"

namespace Lumina
{
    struct FStateTreeActiveState
    {
        uint16              State  = kStateTreeNoIndex;
        EStateTreeRunStatus Status = EStateTreeRunStatus::Running;
    };

    enum class EStateTreeTraceKind : uint8
    {
        Enter,
        Exit,
        Transition,
        Event,
        Finished,
    };

    // One change to a traced instance with the active path right after it, so showing a past time needs no replay.
    struct FStateTreeTraceRecord
    {
        double              Time      = 0.0;
        // The transition index for Transition, the event tag for Event.
        uint32              Value     = 0;
        // The state entered or exited, or a transition's target state.
        uint16              State     = kStateTreeNoIndex;
        EStateTreeTraceKind Kind      = EStateTreeTraceKind::Enter;
        // The state's status for Enter and Exit, the tree's result for Finished.
        EStateTreeRunStatus Status    = EStateTreeRunStatus::Running;
        uint8               PathDepth = 0;
        TArray<FStateTreeActiveState, kStateTreeMaxDepth> Path = {};

        NODISCARD TSpan<const FStateTreeActiveState> GetPath() const { return TSpan<const FStateTreeActiveState>(Path.GetData(), PathDepth); }
    };

    // The recent history of one instance, which exists only while a debugger watches it.
    class FStateTreeTrace
    {
    public:

        static constexpr int32 kCapacity = 512;

        FStateTreeTrace() : Records(kCapacity) {}

        void Record(const FStateTreeTraceRecord& InRecord) { Records.Add(InRecord); }
        void Reset() { Records.Reset(); }

        // Oldest first, the oldest dropping out once the ring is full.
        NODISCARD const TRingBuffer<FStateTreeTraceRecord>& GetRecords() const { return Records; }

        // The newest record at or before Time, none when every record is later.
        NODISCARD RUNTIME_API int32 FindAt(double Time) const;

    private:

        TRingBuffer<FStateTreeTraceRecord> Records;
    };
}
