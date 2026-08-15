#include "RuntimePCH.h"
#include "AnimationGraph.h"

namespace Lumina
{
    void CAnimationGraph::Serialize(FArchive& Ar)
    {
        CObject::Serialize(Ar);

        Ar << Parameters;
        Ar << BoneMasks;
        Ar << StateMachines;
        Ar << Bytecode;
        Ar << NumScalarRegisters;
        Ar << NumPoseRegisters;
        Ar << NumStateSlots;

        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_GRAPH_SYNC_GROUPS)
        {
            Ar << NumSyncGroups;
        }
        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_GRAPH_BYTECODE_VERSION)
        {
            Ar << BytecodeVersion;
        }
        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_CURVES)
        {
            Ar << CurveNames;
            Ar << ClipCurveMaps;
            Ar << BlendSpaceCurveMaps;
        }
        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_GRAPH_MONTAGE_SLOTS)
        {
            Ar << SlotNames;
        }

        ResolveTransitionParameters();
    }

    int32 CAnimationGraph::FindCurveIndex(const FName& Name) const
    {
        for (int32 i = 0; i < (int32)CurveNames.size(); ++i)
        {
            if (CurveNames[i] == Name)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    int32 CAnimationGraph::FindParameterIndex(const FName& Name) const
    {
        for (int32 i = 0; i < (int32)Parameters.size(); ++i)
        {
            if (Parameters[i].Name == Name)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    void CAnimationGraph::ResolveTransitionParameters()
    {
        for (FAnimGraphStateMachine& SM : StateMachines)
        {
            for (FAnimGraphTransition& Transition : SM.Transitions)
            {
                Transition.CachedParamIndex = FindParameterIndex(Transition.ConditionParameter);
            }
        }
    }
}
