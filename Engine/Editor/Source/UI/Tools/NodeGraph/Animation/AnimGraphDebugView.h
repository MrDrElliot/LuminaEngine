#pragma once
#include "Containers/HashTable.h"
#include "Containers/Optional.h"
#include "Containers/RingBuffer.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "imgui.h"

namespace Lumina
{
    class CAnimation;
    class CBlendSpace;
    class CEdGraphNode;
    class CEdNodeGraphPin;

    // The last few seconds of one live value, oldest first.
    using FAnimValueHistory = TRingBuffer<float>;
    constexpr size_t kAnimValueHistoryLength = 240;
    constexpr float  kAnimNodePreviewWidth = 180.0f;

    // Where a state machine is this frame, for its node's preview.
    struct FAnimMachineStatus
    {
        FString Current;
        FString From;
        float   Blend = 1.0f;
    };

    // What the debug target computed this frame, read by node previews and the pose weight overlay.
    struct EDITOR_API FAnimGraphDebugView
    {
        // Copied out of the target's VM state, so a component that moves cannot leave the view dangling.
        TVector<float> Scalars;

        // Refreshed by every compile, which keeps the pin and node pointers valid.
        THashMap<const CEdNodeGraphPin*, uint16> PinRegisters;
        THashMap<const CEdGraphNode*, uint16>    Playheads;

        THashMap<const CEdNodeGraphPin*, FAnimValueHistory> Histories;
        THashMap<const CEdGraphNode*, FAnimMachineStatus>   Machines;

        // How much of the displayed graph's result each pose node supplies, filled only while weights are shown.
        THashMap<const CEdGraphNode*, float> NodeWeights;

        bool bLive = false;
        bool bShowWeights = false;

        // An input reads its own register when it has one, else whatever its wire carries.
        TOptional<float> ReadPin(const CEdNodeGraphPin* Pin) const;
        TOptional<float> ReadPlayhead(const CEdGraphNode* Node) const;
        const FAnimValueHistory* FindHistory(const CEdNodeGraphPin* Pin) const;

        // 1 for a node outside the weighted set, so a graph with weights off draws as it always has.
        float GetNodeWeight(const CEdGraphNode* Node) const;
    };

    namespace AnimDebugDraw
    {
        // Each takes the width it may use at the cursor and submits one item of its own height.
        void Plot(const FAnimValueHistory& Values, ImVec2 Size, ImU32 Color);
        void ClipTimeline(const CAnimation* Clip, TOptional<float> Time, float Width);
        void BlendSpace(const CBlendSpace* Space, TOptional<float> X, TOptional<float> Y, float Width);
        void Transfer(float InMin, float InMax, float OutMin, float OutMax, bool bClamp, TOptional<float> Input, float Width);
        void MachineStatus(const FAnimMachineStatus& Status, float Width);
        void SeamDecay(float Duration, TOptional<float> Elapsed, float Width);
    }
}
