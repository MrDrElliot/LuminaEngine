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

}
