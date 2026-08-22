#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "AnimationGraph.h"

#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"

namespace Lumina
{
    EAnimParamValueType AnimParamValueTypeFromProperty(const FProperty* Property)
    {
        if (Property == nullptr)
        {
            return EAnimParamValueType::Unresolved;
        }

        switch (Property->TypeFlags)
        {
        case EPropertyTypeFlags::Float:  return EAnimParamValueType::Float;
        case EPropertyTypeFlags::Double: return EAnimParamValueType::Double;
        case EPropertyTypeFlags::Bool:   return EAnimParamValueType::Bool;
        case EPropertyTypeFlags::Int8:   return EAnimParamValueType::Int8;
        case EPropertyTypeFlags::Int16:  return EAnimParamValueType::Int16;
        case EPropertyTypeFlags::Int32:  return EAnimParamValueType::Int32;
        case EPropertyTypeFlags::Int64:  return EAnimParamValueType::Int64;
        case EPropertyTypeFlags::UInt8:  return EAnimParamValueType::UInt8;
        case EPropertyTypeFlags::UInt16: return EAnimParamValueType::UInt16;
        case EPropertyTypeFlags::UInt32: return EAnimParamValueType::UInt32;
        case EPropertyTypeFlags::UInt64: return EAnimParamValueType::UInt64;
        case EPropertyTypeFlags::Object: return EAnimParamValueType::Object;

        case EPropertyTypeFlags::Enum:
            {
                const FEnumProperty* AsEnum = static_cast<const FEnumProperty*>(Property);
                return AnimParamValueTypeFromProperty(AsEnum->GetInnerProperty());
            }

        default: return EAnimParamValueType::Unresolved;
        }
    }

    void CAnimationGraph::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Animation");
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
        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_GRAPH_OBJECT_PARAMETERS)
        {
            Ar << NumObjectRegisters;
        }
        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_GRAPH_POSE_SNAPSHOTS)
        {
            Ar << PoseSnapshotNames;
        }
        if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ANIM_GRAPH_INERTIALIZATION_NODES)
        {
            Ar << NumInertializerNodes;
            Ar << NumDeadBlendNodes;
        }

        ResolveTransitionParameters();
        LinkParameters();
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

    int32 CAnimationGraph::FindObjectParameterIndex(const FName& Name) const
    {
        for (int32 i = 0; i < (int32)ObjectParameters.size(); ++i)
        {
            if (ObjectParameters[i].Name == Name)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    void CAnimationGraph::LinkParameters()
    {
        ParamBindings.assign(Parameters.size(), FAnimGraphParamBinding());
        ObjectParamBindings.assign(ObjectParameters.size(), FAnimGraphParamBinding());

        CStruct* Struct = GetParameterStruct();
        if (Struct == nullptr)
        {
            if (!Parameters.empty() || !ObjectParameters.empty())
            {
                LOG_WARN("Anim graph '{}' reads {} parameter(s) but has no ParameterStruct assigned; none of them will ever receive a value.",
                         GetName().c_str(), (int32)(Parameters.size() + ObjectParameters.size()));
            }
            return;
        }

        const auto Bind = [Struct](const FName& Name, bool bWantObject) -> FAnimGraphParamBinding
        {
            FAnimGraphParamBinding Binding;
            if (Name.IsNone())
            {
                return Binding;
            }

            FProperty* Property = Struct->GetProperty(Name);

            // A raw offset read would bypass an accessor, so refuse rather than return a stale value.
            if (Property == nullptr || Property->HasSetterOrGetter())
            {
                return Binding;
            }

            const EAnimParamValueType Type = AnimParamValueTypeFromProperty(Property);
            const bool bIsObject = Type == EAnimParamValueType::Object;
            if (Type == EAnimParamValueType::Unresolved || bIsObject != bWantObject)
            {
                return Binding;
            }

            Binding.Offset = Property->Offset;
            Binding.Type   = Type;
            return Binding;
        };

        const auto WarnUnresolved = [this, Struct](const FName& Name)
        {
            if (!Name.IsNone())
            {
                LOG_WARN("Anim graph '{}': parameter '{}' does not resolve to a readable field on '{}'; it will hold its default.",
                         GetName().c_str(), Name.ToString().c_str(), Struct->GetName().c_str());
            }
        };

        for (SIZE_T i = 0; i < Parameters.size(); ++i)
        {
            ParamBindings[i] = Bind(Parameters[i].Name, false);
            if (!ParamBindings[i].IsResolved())
            {
                WarnUnresolved(Parameters[i].Name);
            }
        }

        for (SIZE_T i = 0; i < ObjectParameters.size(); ++i)
        {
            ObjectParamBindings[i] = Bind(ObjectParameters[i].Name, true);
            if (!ObjectParamBindings[i].IsResolved())
            {
                WarnUnresolved(ObjectParameters[i].Name);
            }
        }
    }

    void CAnimationGraph::ResolveTransitionParameters()
    {
        for (FAnimGraphStateMachine& SM : StateMachines)
        {
            for (FAnimGraphTransition& Transition : SM.Transitions)
            {
                for (FAnimGraphTransitionTerm& Term : Transition.Terms)
                {
                    Term.CachedIndex = (Term.ConditionSource == EAnimTransitionSource::Curve)
                        ? FindCurveIndex(Term.Name)
                        : FindParameterIndex(Term.Name);
                }
            }
        }
    }
}
