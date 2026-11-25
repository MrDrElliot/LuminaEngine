#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "AnimationGraph.h"

#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Containers/StringFormat.h"
#include "Core/Math/Hash/Hash.h"
#include "World/Entity/Components/Component.h"

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
        return Algo::IndexOf(CurveNames, Name);
    }

    int32 CAnimationGraph::FindParameterIndex(const FName& Name) const
    {
        return Algo::IndexOf(Parameters, Name, &FAnimGraphParameter::Name);
    }

    int32 CAnimationGraph::FindObjectParameterIndex(const FName& Name) const
    {
        return Algo::IndexOf(ObjectParameters, Name, &SAnimGraphObjectParameter::Name);
    }

    int32 CAnimationGraph::FindInput(FName Name) const
    {
        return Algo::IndexOf(Inputs, Name, &SAnimGraphInput::Name);
    }

    bool CAnimationGraph::ResolveParameterPath(FName Path, bool bWantObject, FAnimGraphParamBinding& Out, FString* OutError, const FProperty** OutLeaf) const
    {
        Out = FAnimGraphParamBinding();
        const auto Fail = [OutError](FString Message)
        {
            if (OutError != nullptr)
            {
                *OutError = Move(Message);
            }
            return false;
        };

        if (Path.IsNone())
        {
            return Fail("No parameter named");
        }

        const FString Text = Path.ToString();
        TVector<FStringView> Parts;
        for (FStringView Rest(Text); !Rest.IsEmpty();)
        {
            const int32 Dot = Rest.Find('.');
            Parts.Add(Dot == Constants::kIndexNone ? Rest : Rest.Left(Dot));
            Rest = Dot == Constants::kIndexNone ? FStringView() : Rest.Mid(Dot + 1);
        }

        // A first part naming an input reads that component, and anything else reads the instance's own parameter block.
        const CStruct* Struct = GetParameterStruct();
        int32 First = 0;
        if (Parts.Num() > 1)
        {
            const int32 InputIndex = FindInput(FName(Parts[0]));
            if (InputIndex != Constants::kIndexNone && InputIndex < kMaxAnimGraphInputs)
            {
                Struct = InputIndex < InputStructs.Num() ? InputStructs[InputIndex] : nullptr;
                if (Struct == nullptr)
                {
                    return Fail(Format("Input '{}' names component '{}', which does not exist", Inputs[InputIndex].Name, Inputs[InputIndex].Component));
                }
                Out.Input = (uint16)InputIndex;
                First = 1;
            }
        }
        if (Struct == nullptr)
        {
            return Fail(Format("'{}' names no input, and the graph has no parameter struct", Parts[0]));
        }

        uint32 Offset = 0;
        for (int32 i = First; i < Parts.Num(); ++i)
        {
            const FProperty* Property = Struct->GetProperty(FName(Parts[i]));
            if (Property == nullptr)
            {
                return Fail(Format("'{}' has no field '{}'", Struct->GetName(), Parts[i]));
            }

            // A raw offset read would bypass an accessor, so refuse rather than return a stale value.
            if (Property->HasSetterOrGetter())
            {
                return Fail(Format("'{}' reads through an accessor, which a graph cannot", Parts[i]));
            }

            if (i == Parts.Num() - 1)
            {
                const EAnimParamValueType Type = AnimParamValueTypeFromProperty(Property);
                if (Type == EAnimParamValueType::Unresolved)
                {
                    return Fail(Format("'{}' is not a number, bool, enum or asset", Parts[i]));
                }
                if ((Type == EAnimParamValueType::Object) != bWantObject)
                {
                    return Fail(bWantObject ? Format("'{}' is a value, not an asset", Parts[i]) : Format("'{}' is an asset, not a value", Parts[i]));
                }
                Out.Offset = Offset + (uint32)Property->Offset;
                Out.Type   = Type;
                if (OutLeaf != nullptr)
                {
                    *OutLeaf = Property;
                }
                return true;
            }

            if (Property->GetType() == EPropertyTypeFlags::Struct)
            {
                Offset += (uint32)Property->Offset;
                Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
            }
            else if (Property->GetType() == EPropertyTypeFlags::Object && Out.HopOffset == kAnimNoHop)
            {
                Out.HopOffset = Offset + (uint32)Property->Offset;
                Offset = 0;
                Struct = static_cast<const FObjectProperty*>(Property)->GetPropertyClass();
            }
            else
            {
                return Fail(Property->GetType() == EPropertyTypeFlags::Object
                    ? Format("'{}' is a second asset reference, and a path follows only one", Parts[i])
                    : Format("'{}' has no fields to read", Parts[i]));
            }

            if (Struct == nullptr)
            {
                return Fail(Format("'{}' has no reflected type", Parts[i]));
            }
        }
        return Fail("The path ends before naming a field");
    }

    void CAnimationGraph::RefreshInputStructs(TVector<FString>* OutProblems)
    {
        InputStructs.Reset();
        for (const SAnimGraphInput& Input : Inputs)
        {
            CStruct* Struct = Input.Component.IsNone() ? nullptr : FindComponentStruct(Input.Component.ToString());
            InputStructs.Add(Struct);
            if (Struct == nullptr && !Input.Component.IsNone() && OutProblems != nullptr)
            {
                OutProblems->Add(Format("input '{}' names component '{}', which does not exist; its fields hold their defaults", Input.Name, Input.Component));
            }
        }
        if (Inputs.Num() > kMaxAnimGraphInputs && OutProblems != nullptr)
        {
            OutProblems->Add(Format("{} inputs, and only the first {} are read", Inputs.Num(), kMaxAnimGraphInputs));
        }
    }

    void CAnimationGraph::LinkParameters()
    {
        TVector<FString> Problems;
        RefreshInputStructs(&Problems);

        const auto Bind = [this, &Problems](const FName& Name, bool bWantObject) -> FAnimGraphParamBinding
        {
            FAnimGraphParamBinding Binding;
            FString Error;
            if (!Name.IsNone() && !ResolveParameterPath(Name, bWantObject, Binding, &Error))
            {
                Problems.Add(Format("parameter '{}' holds its default, because {}", Name, Error));
            }
            return Binding;
        };

        ParamBindings.SetNum(Parameters.Num());
        for (int32 i = 0; i < Parameters.Num(); ++i)
        {
            ParamBindings[i] = Bind(Parameters[i].Name, false);
        }

        ObjectParamBindings.SetNum(ObjectParameters.Num());
        for (int32 i = 0; i < ObjectParameters.Num(); ++i)
        {
            ObjectParamBindings[i] = Bind(ObjectParameters[i].Name, true);
        }

        // An editor relinks every frame and a reload relinks per world, so the same problems are reported once until they change.
        size_t ProblemsHash = 0;
        for (const FString& Problem : Problems)
        {
            Hash::HashCombine(ProblemsHash, GetTypeHash(Problem));
        }
        if (ProblemsHash != ReportedLinkProblems)
        {
            ReportedLinkProblems = ProblemsHash;
            for (const FString& Problem : Problems)
            {
                LOG_WARN("Anim graph '{}': {}.", *GetName(), Problem);
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

            // An any-state edge sits in every list at its authored position, so the first passing edge stays the same one.
            const int32 NumStates = SM.StatePoseRegisters.Num();
            SM.TransitionsByState.Reset();
            SM.TransitionsByStateStart.Init(0u, (SIZE_T)NumStates + 1);
            for (int32 StateIndex = 0; StateIndex < NumStates; ++StateIndex)
            {
                SM.TransitionsByStateStart[StateIndex] = (uint32)SM.TransitionsByState.Num();
                for (int32 TransitionIndex = 0; TransitionIndex < SM.Transitions.Num(); ++TransitionIndex)
                {
                    const int32 From = SM.Transitions[TransitionIndex].FromState;
                    if (From == StateIndex || From < 0)
                    {
                        SM.TransitionsByState.Add((uint16)TransitionIndex);
                    }
                }
            }
            SM.TransitionsByStateStart[NumStates] = (uint32)SM.TransitionsByState.Num();
        }

        // Loading and compiling both end here, so the decoded plan always matches the bytecode.
        LowerAnimGraphPlan(*this, Plan);
        PruneAnimGraphPlanForCrowd(*this, Plan, CrowdPlan);
    }
}
