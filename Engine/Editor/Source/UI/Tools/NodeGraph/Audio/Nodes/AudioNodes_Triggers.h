#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphNode.h"
#include "AudioNodes_Triggers.generated.h"

namespace Lumina
{
    REFLECT()
    class CAudioNode_TriggerRepeat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "TriggerRepeat"; }
        FStringView GetNodeDisplayName() const override { return "Trigger Repeat"; }
        FStringView GetNodeTooltip() const override { return "Fires on a period until stopped. Timing is sample accurate."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Trigger Repeat", ClampMin = 0.001f)
        float Period = 0.5f;
    };

    REFLECT()
    class CAudioNode_TriggerDelay : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "TriggerDelay"; }
        FStringView GetNodeDisplayName() const override { return "Trigger Delay"; }
        FStringView GetNodeTooltip() const override { return "Re-fires each input trigger later. Sixteen can be in flight at once."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Trigger Delay", ClampMin = 0.0f)
        float Delay = 0.25f;
    };

    REFLECT()
    class CAudioNode_TriggerOnce : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "TriggerOnce"; }
        FStringView GetNodeDisplayName() const override { return "Trigger Once"; }
        FStringView GetNodeTooltip() const override { return "Passes the first trigger and swallows the rest until reset."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }
    };

    REFLECT()
    class CAudioNode_TriggerAny : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "TriggerAny"; }
        FStringView GetNodeDisplayName() const override { return "Trigger Any"; }
        FStringView GetNodeTooltip() const override { return "Merges up to four trigger streams."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }
    };

    REFLECT()
    class CAudioNode_TriggerCounter : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "TriggerCounter"; }
        FStringView GetNodeDisplayName() const override { return "Trigger Counter"; }
        FStringView GetNodeTooltip() const override { return "Counts triggers and fires Wrapped when the count reaches Reset Count."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Trigger Counter", ClampMin = 0)
        int32 ResetCount = 0;
    };

    REFLECT()
    class CAudioNode_RandomFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Random.Float"; }
        FStringView GetNodeDisplayName() const override { return "Random (Float)"; }
        FStringView GetNodeTooltip() const override { return "Picks a new value in the range each time the trigger fires."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Random (Float)")
        float Min = 0.0f;

        PROPERTY(Editable, Category = "Random (Float)")
        float Max = 1.0f;

        PROPERTY(Editable, Category = "Random (Float)")
        int32 Seed = 1;
    };

    REFLECT()
    class CAudioNode_TriggerOnThreshold : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "TriggerOnThreshold"; }
        FStringView GetNodeDisplayName() const override { return "Trigger On Threshold"; }
        FStringView GetNodeTooltip() const override { return "Fires when a value crosses the threshold, once per crossing."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Trigger On Threshold")
        float Value = 0.0f;

        PROPERTY(Editable, Category = "Trigger On Threshold")
        float Threshold = 0.5f;
    };

    REFLECT()
    class CAudioNode_TriggerSelect : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "TriggerSelect"; }
        FStringView GetNodeDisplayName() const override { return "Trigger Select"; }
        FStringView GetNodeTooltip() const override { return "Sends each trigger to one of up to four outputs, in turn, at random, or to the one Index names."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Trigger Select")
        EAudioPickMode Mode = EAudioPickMode::Sequential;

        PROPERTY(Editable, Category = "Trigger Select", ClampMin = 0, ClampMax = 3)
        int32 Index = 0;

        /** How many of the outputs take part. */
        PROPERTY(Editable, Category = "Trigger Select", ClampMin = 1, ClampMax = 4)
        int32 Count = 4;

        PROPERTY(Editable, Category = "Trigger Select")
        int32 Seed = 1;
    };

    REFLECT()
    class CAudioNode_TriggerProbability : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "TriggerProbability"; }
        FStringView GetNodeDisplayName() const override { return "Trigger Probability"; }
        FStringView GetNodeTooltip() const override { return "Passes each trigger with the given chance, and sends the rest to On Fail."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Trigger Probability", ClampMin = 0.0f, ClampMax = 1.0f)
        float Probability = 0.5f;

        PROPERTY(Editable, Category = "Trigger Probability")
        int32 Seed = 1;
    };

    REFLECT()
    class CAudioNode_TriggerGate : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "TriggerGate"; }
        FStringView GetNodeDisplayName() const override { return "Trigger Gate"; }
        FStringView GetNodeTooltip() const override { return "Lets triggers through only while open."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Trigger Gate")
        bool StartOpen = true;
    };

    REFLECT()
    class CAudioNode_TriggerOnBool : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "TriggerOnBool"; }
        FStringView GetNodeDisplayName() const override { return "Trigger On Bool"; }
        FStringView GetNodeTooltip() const override { return "Fires when a bool changes. The starting value fires nothing."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Trigger On Bool")
        bool Value = false;
    };

    REFLECT()
    class CAudioNode_RandomInt : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Random.Int"; }
        FStringView GetNodeDisplayName() const override { return "Random (Int)"; }
        FStringView GetNodeTooltip() const override { return "Picks a whole number from Min to Max, inclusive, each time the trigger fires."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Random (Int)")
        int32 Min = 0;

        PROPERTY(Editable, Category = "Random (Int)")
        int32 Max = 3;

        PROPERTY(Editable, Category = "Random (Int)")
        int32 Seed = 1;

        PROPERTY(Editable, Category = "Random (Int)")
        bool NoRepeat = true;
    };

    REFLECT()
    class CAudioNode_BeatClock : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "BeatClock"; }
        FStringView GetNodeDisplayName() const override { return "Beat Clock"; }
        FStringView GetNodeTooltip() const override { return "Musical clock firing on every subdivision, beat and bar, sample accurate."; }
        FFixedString GetNodeCategory() const override { return "Triggers"; }

        PROPERTY(Editable, Category = "Beat Clock", ClampMin = 1.0f)
        float BPM = 120.0f;

        PROPERTY(Editable, Category = "Beat Clock", ClampMin = 1)
        int32 BeatsPerBar = 4;

        /** Ticks per beat. */
        PROPERTY(Editable, Category = "Beat Clock", ClampMin = 1)
        int32 Subdivision = 4;
    };
}
