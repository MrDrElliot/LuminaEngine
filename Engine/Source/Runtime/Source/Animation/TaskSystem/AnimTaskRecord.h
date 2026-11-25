#pragma once

#include "Animation/TaskSystem/AnimTask.h"
#include "Containers/HashTable.h"
#include "Memory/SmartPtr.h"
#include "Core/Threading/Mutex.h"

namespace Lumina
{
    struct FAnimGraphVMState;

    // A task list with its per-frame values taken out, shared by every character whose update took the same path.
    struct FAnimTopology
    {
        TVector<FAnimTask> Tasks;

        // Per task, which of the character's own smoothing or snapshot records it uses, since those addresses differ per character.
        TVector<int32> StateRefs;

        // Where each task's per-frame values start in a record's payload.
        TVector<int32> PayloadOffsets;

        int16 OutputTask    = FAnimTask::NoTask;
        bool  bLockRoot     = false;
        int32 RootBoneIndex = Constants::kIndexNone;
        int32 PayloadFloats = 0;
    };

    // A crowd topology as weighted clip samples, so a backend evaluates a character as a sum instead of walking its task graph.
    struct FAnimSampleProgram
    {
        struct FSample
        {
            // Null samples the bind pose.
            const CAnimation* Clip = nullptr;
            int32 TimeFloat   = Constants::kIndexNone;
            int32 FirstFactor = 0;
            int32 NumFactors  = 0;
        };

        // A blend between the sample and the output, which weights it by alpha as the blend's second input and by one minus alpha as its first.
        struct FFactor
        {
            int32 AlphaFloat   = 0;
            bool  bSecondInput = false;
        };

        // A seam smoothing the pose below it, which a crowd backend replays as a decaying offset rather than history.
        struct FTransition
        {
            int32 ElapsedFloat = 0;
            int32 FlagsFloat   = 0;
            int32 StateRef     = 0;
        };

        TVector<FSample>     Samples;
        TVector<FFactor>     Factors;
        TVector<FTransition> Transitions;

        // False when a task has no weighted-sum form, such as a masked layer or an IK solve.
        bool bExpressible = false;
    };

    struct FAnimWeightedSample
    {
        const CAnimation* Clip   = nullptr;
        float             Time   = 0.0f;
        float             Weight = 0.0f;
    };

    struct FAnimTopologyEntry
    {
        FAnimTopology      Topology;
        FAnimSampleProgram Program;
    };

    // The topologies a crowd has reached, read by every worker each update and written once per new path, owned by whichever system runs the crowd.
    class RUNTIME_API FAnimTopologyCache
    {
    public:

        const FAnimTopologyEntry* Find(uint64 Key) const;

        // Builds the entry from the update that reached the key, its tasks still in hand, when no other worker got there first.
        const FAnimTopologyEntry& FindOrAdd(uint64 Key, const FAnimTaskList& Tasks, const FAnimGraphVMState& State);

        int32 Num() const;

    private:

        TRwLock<THashMap<uint64, TUniquePtr<FAnimTopologyEntry>>> Entries;
    };

    // One character's update as the topology it took plus the values that change frame to frame.
    struct FAnimTaskRecord
    {
        uint64         TopologyKey = 0;
        TVector<float> Payload;
    };

    namespace Anim
    {
        // Appends the fields this task's type rewrites every frame, in the order ExpandTaskList reads them back.
        RUNTIME_API void AppendTaskPayload(const FAnimTask& Task, TVector<float>& Payload);

        // Equal keys mean equal topologies, so a crowd shares one template per path instead of a task list per character.
        RUNTIME_API void RecordTaskList(const FAnimTaskList& Tasks, const FAnimGraphVMState& State, FAnimTaskRecord& Out);

        RUNTIME_API void MakeTopology(const FAnimTaskList& Tasks, const FAnimGraphVMState& State, FAnimTopology& Out);

        // Rebuilds the exact task list a record came from, pointing at this character's own records.
        RUNTIME_API void ExpandTaskList(const FAnimTopology& Topology, const FAnimTaskRecord& Record, FAnimGraphVMState& State, FAnimTaskList& Out);

        RUNTIME_API void MakeSampleProgram(const FAnimTopology& Topology, FAnimSampleProgram& Out);

        // A character's samples with their final weights, leaving out any a blend has weighted to nothing.
        RUNTIME_API void EvaluateSampleProgram(const FAnimSampleProgram& Program, const FAnimTaskRecord& Record, TVector<FAnimWeightedSample>& Out);
    }
}
