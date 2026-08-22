#include "RuntimePCH.h"
#include "Future.h"

#include "Memory/Memory.h"

namespace Lumina::FutureDetail
{
    namespace
    {
        struct FContinuationJob
        {
            TMoveOnlyFunction<void()> Fn;
        };

        void RunContinuationJob(void* Arg, uint32 /*Worker*/)
        {
            FContinuationJob* Job = static_cast<FContinuationJob*>(Arg);
            Job->Fn();
            Memory::Delete(Job);
        }
    }

    void DispatchContinuation(TMoveOnlyFunction<void()>&& Fn)
    {
        FContinuationJob* Job = Memory::New<FContinuationJob>();
        Job->Fn = Move(Fn);
        // The continuation owns any further sequencing through the future it sets.
        Jobs::RunJob(&RunContinuationJob, Job, Jobs::EJobPriority::Normal, nullptr, "Future::Continuation");
    }
}
