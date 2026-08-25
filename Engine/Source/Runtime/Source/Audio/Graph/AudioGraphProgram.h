#pragma once

#include "AudioGraphTypes.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FArchive;

    /** Starting value written into a slot on reset, covering both literals and unconnected pin defaults. */
    struct FAudioGraphSlotInit
    {
        EAudioGraphType Type = EAudioGraphType::Invalid;
        uint16          Slot = kAudioGraphInvalidSlot;

        float           FloatValue = 0.0f;
        int32           IntValue   = 0;
        bool            BoolValue  = false;

        /** Index into the owning asset's ReferencedWaves, or INDEX_NONE. */
        int32           WaveIndex  = -1;

        friend RUNTIME_API FArchive& operator << (FArchive& Ar, FAudioGraphSlotInit& Data);
    };

    /** One operator to build, with every pin already resolved to an arena slot. */
    struct FAudioGraphNodeInstance
    {
        /** Registry key of the operator class, e.g. "Sine". */
        FName OperatorName;

        /** Arena slot per declared input pin, in the registered descriptor's order. */
        TVector<uint16> InputSlots;

        TVector<uint16> OutputSlots;

        /** Editor node id this came from, so a debug overlay can map execution back to the canvas. */
        int64 SourceNodeID = 0;

        friend RUNTIME_API FArchive& operator << (FArchive& Ar, FAudioGraphNodeInstance& Data);
    };

    /** A named value gameplay can read or write on a live instance. */
    struct FAudioGraphParameterDecl
    {
        FName           Name;
        EAudioGraphType Type = EAudioGraphType::Invalid;
        uint16          Slot = kAudioGraphInvalidSlot;

        float           DefaultFloat = 0.0f;
        int32           DefaultInt   = 0;
        bool            DefaultBool  = false;

        friend RUNTIME_API FArchive& operator << (FArchive& Ar, FAudioGraphParameterDecl& Data);
    };

    /** Per type slot counts, indexed by EAudioGraphType. */
    struct FAudioGraphSlotCounts
    {
        static constexpr uint32 NumTypes = (uint32)EAudioGraphType::Wave + 1;

        uint16 Counts[NumTypes] = {};

        uint16 Get(EAudioGraphType Type) const { return Counts[(uint32)Type]; }
        void Set(EAudioGraphType Type, uint16 Value) { Counts[(uint32)Type] = Value; }

        /** Reserves the next slot of Type and returns it. */
        uint16 Allocate(EAudioGraphType Type) { return Counts[(uint32)Type]++; }

        friend RUNTIME_API FArchive& operator << (FArchive& Ar, FAudioGraphSlotCounts& Data);
    };

    /** Flat executable form of an audio graph, with nodes in execution order and every wire an arena slot. */
    struct RUNTIME_API FAudioGraphProgram
    {
        uint16 Version = kAudioGraphProgramVersion;

        /** Topologically sorted, so executing front to back satisfies every dependency. */
        TVector<FAudioGraphNodeInstance> Nodes;

        TVector<FAudioGraphSlotInit> SlotInits;

        TVector<FAudioGraphParameterDecl> Inputs;
        TVector<FAudioGraphParameterDecl> Outputs;

        FAudioGraphSlotCounts SlotCounts;

        /** Audio slots the graph's left and right output pins land in. */
        uint16 OutputLeftSlot  = kAudioGraphInvalidSlot;
        uint16 OutputRightSlot = kAudioGraphInvalidSlot;

        /** Trigger slot the graph raises when the voice has nothing left to play. */
        uint16 FinishedSlot = kAudioGraphInvalidSlot;

        bool IsValid() const { return Version == kAudioGraphProgramVersion && !Nodes.empty(); }

        void Reset();

        const FAudioGraphParameterDecl* FindInput(const FName& Name) const;
        const FAudioGraphParameterDecl* FindOutput(const FName& Name) const;

        friend RUNTIME_API FArchive& operator << (FArchive& Ar, FAudioGraphProgram& Data);
    };
}
