#include "RuntimePCH.h"
#include "AudioGraphProgram.h"

#include "Core/Serialization/Archiver.h"

namespace Lumina
{
    const char* ToString(EAudioGraphType Type)
    {
        switch (Type)
        {
        case EAudioGraphType::Audio:   return "Audio";
        case EAudioGraphType::Float:   return "Float";
        case EAudioGraphType::Int32:   return "Int32";
        case EAudioGraphType::Bool:    return "Bool";
        case EAudioGraphType::Trigger: return "Trigger";
        case EAudioGraphType::Wave:    return "Wave";
        default:                       return "Invalid";
        }
    }

    FArchive& operator << (FArchive& Ar, FAudioGraphSlotInit& Data)
    {
        Ar << Data.Type;
        Ar << Data.Slot;
        Ar << Data.FloatValue;
        Ar << Data.IntValue;
        Ar << Data.BoolValue;
        Ar << Data.WaveIndex;
        return Ar;
    }

    FArchive& operator << (FArchive& Ar, FAudioGraphNodeInstance& Data)
    {
        Ar << Data.OperatorName;
        Ar << Data.InputSlots;
        Ar << Data.OutputSlots;
        Ar << Data.SourceNodeID;
        return Ar;
    }

    FArchive& operator << (FArchive& Ar, FAudioGraphParameterDecl& Data)
    {
        Ar << Data.Name;
        Ar << Data.Type;
        Ar << Data.Slot;
        Ar << Data.DefaultFloat;
        Ar << Data.DefaultInt;
        Ar << Data.DefaultBool;
        return Ar;
    }

    FArchive& operator << (FArchive& Ar, FAudioGraphSlotCounts& Data)
    {
        for (uint32 Index = 0; Index < FAudioGraphSlotCounts::NumTypes; ++Index)
        {
            Ar << Data.Counts[Index];
        }
        return Ar;
    }

    void FAudioGraphProgram::Reset()
    {
        Version = kAudioGraphProgramVersion;
        Nodes.clear();
        SlotInits.clear();
        Inputs.clear();
        Outputs.clear();
        SlotCounts = FAudioGraphSlotCounts();
        OutputLeftSlot  = kAudioGraphInvalidSlot;
        OutputRightSlot = kAudioGraphInvalidSlot;
        FinishedSlot    = kAudioGraphInvalidSlot;
    }

    const FAudioGraphParameterDecl* FAudioGraphProgram::FindInput(const FName& Name) const
    {
        for (const FAudioGraphParameterDecl& Decl : Inputs)
        {
            if (Decl.Name == Name)
            {
                return &Decl;
            }
        }
        return nullptr;
    }

    const FAudioGraphParameterDecl* FAudioGraphProgram::FindOutput(const FName& Name) const
    {
        for (const FAudioGraphParameterDecl& Decl : Outputs)
        {
            if (Decl.Name == Name)
            {
                return &Decl;
            }
        }
        return nullptr;
    }

    FArchive& operator << (FArchive& Ar, FAudioGraphProgram& Data)
    {
        Ar << Data.Version;

        if (Ar.IsReading() && Data.Version != kAudioGraphProgramVersion)
        {
            // A program from another layout cannot be read field by field, so drop it and wait for a recompile.
            Data.Reset();
            Data.Version = 0;
            return Ar;
        }

        Ar << Data.Nodes;
        Ar << Data.SlotInits;
        Ar << Data.Inputs;
        Ar << Data.Outputs;
        Ar << Data.SlotCounts;
        Ar << Data.OutputLeftSlot;
        Ar << Data.OutputRightSlot;
        Ar << Data.FinishedSlot;
        return Ar;
    }
}
