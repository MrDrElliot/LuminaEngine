#pragma once

#include "Containers/Vector.h"

namespace Lumina
{
    class CAnimationGraph;

    // One decoded bytecode instruction, with integer operands in the order the update reads them.
    struct FAnimPlanOp
    {
        static constexpr int32 MaxOperands = 14;

        uint8  Op = 0;
        uint16 U[MaxOperands] = {};

        // First float in the plan's immediates, or first entry in its jumps for EnterStateMachine.
        uint32 Extra = 0;
    };

    struct FAnimPlanConstant
    {
        uint16 Register = 0;
        float  Value    = 0.0f;
    };

    struct FAnimPlanLoad
    {
        uint16 Register = 0;
        uint16 Source   = 0;
    };

    // The graph's bytecode decoded once at load, with every load whose register has no other writer lifted to the start of an update.
    struct FAnimGraphPlan
    {
        // Marks a jump whose byte offset was not an instruction boundary, which fails every range check.
        static constexpr uint32 InvalidJump = 0xFFFFFFFFu;

        TVector<FAnimPlanOp> Ops;
        TVector<float>       Immediates;

        // EnterStateMachine tables as op indices, the end of all state code first, then a begin and end per state.
        TVector<uint32> Jumps;

        TVector<FAnimPlanConstant> Constants;
        TVector<FAnimPlanLoad>     ParameterLoads;
        TVector<FAnimPlanLoad>     ObjectParameterLoads;
        TVector<FAnimPlanLoad>     ObjectConstantLoads;

        void Reset() { *this = FAnimGraphPlan(); }
    };

    // Bytecode offsets parallel to a plan's ops and lifted loads, so a tool can tell which node wrote each.
    struct FAnimPlanSources
    {
        TVector<uint32> Ops;
        TVector<uint32> Constants;
        TVector<uint32> ParameterLoads;
        TVector<uint32> ObjectParameterLoads;
        TVector<uint32> ObjectConstantLoads;
    };

    RUNTIME_API void LowerAnimGraphPlan(const CAnimationGraph& Graph, FAnimGraphPlan& OutPlan, FAnimPlanSources* OutSources = nullptr);

    // What the crowd plan does with one op of the full plan.
    enum class EAnimCrowdOpFate : uint8
    {
        Kept,
        Bypassed,
        Removed,
    };

    // The crowd tier's plan from the same graph, overlays (IK, look-at, bone edits, additive and masked layers) passing their base through and everything only they used removed.
    RUNTIME_API void PruneAnimGraphPlanForCrowd(const CAnimationGraph& Graph, const FAnimGraphPlan& Full, FAnimGraphPlan& OutPlan, TVector<EAnimCrowdOpFate>* OutFates = nullptr);
}
