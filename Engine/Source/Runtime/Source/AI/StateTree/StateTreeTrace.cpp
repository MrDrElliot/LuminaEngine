#include "RuntimePCH.h"
#include "StateTreeExecution.h"

namespace Lumina
{
    int32 FStateTreeTrace::FindAt(double Time) const
    {
        for (int32 Index = Records.Num() - 1; Index >= 0; --Index)
        {
            if (Records[Index].Time <= Time)
            {
                return Index;
            }
        }
        return Constants::kIndexNone;
    }

    void FStateTreeInstance::SetTraced(bool bTraced)
    {
#if !defined(LE_SHIPPING)
        if (!bTraced)
        {
            Trace.reset();
        }
        else if (Trace == nullptr)
        {
            Trace = MakeUnique<FStateTreeTrace>();
        }
#endif
    }

    const FStateTreeTrace* FStateTreeInstance::GetTrace() const
    {
#if !defined(LE_SHIPPING)
        return Trace.get();
#else
        return nullptr;
#endif
    }

#if !defined(LE_SHIPPING)
    void FStateTreeInstance::RecordTrace(double Time, EStateTreeTraceKind Kind, uint16 State, EStateTreeRunStatus StateStatus, uint32 Value, int32 Depth) const
    {
        FStateTreeTraceRecord Record = { Time, Value, State, Kind, StateStatus, (uint8)Depth };
        for (int32 Level = 0; Level < Depth; ++Level)
        {
            Record.Path[Level] = ActivePath[Level];
        }
        Trace->Record(Record);
    }
#endif
}
