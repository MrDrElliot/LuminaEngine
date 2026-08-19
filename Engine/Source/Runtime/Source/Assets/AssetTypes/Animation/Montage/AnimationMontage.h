#pragma once

#include "Animation/AnimNotify.h"
#include "Core/Object/InstancedStruct.h"

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimationMontage.generated.h"

namespace Lumina
{
    class CAnimation;
    class CSkeleton;

    // One clip placed on a slot track's timeline; segments never cross-fade with each other.
    REFLECT()
    struct RUNTIME_API SAnimMontageSegment
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Segment")
        TObjectPtr<CAnimation> Animation;

        /** Where the segment begins on the montage timeline. */
        PROPERTY(Editable, Category = "Segment", Units = "s", ClampMin = 0.0f)
        float StartTime = 0.0f;

        /** Trim into the clip: playback starts here rather than at the clip's own zero. */
        PROPERTY(Editable, Category = "Segment", Units = "s", ClampMin = 0.0f)
        float ClipStartTime = 0.0f;

        /** In-clip time playback stops at. Zero or less means the clip's full duration. */
        PROPERTY(Editable, Category = "Segment", Units = "s")
        float ClipEndTime = 0.0f;

        PROPERTY(Editable, Category = "Segment", ClampMin = 0.01f)
        float PlayRate = 1.0f;

        /** How many times the trimmed clip repeats before the segment ends. */
        PROPERTY(Editable, Category = "Segment", ClampMin = 1)
        int32 LoopCount = 1;

        /** Trimmed length of one pass through the clip, before LoopCount and PlayRate. */
        float GetTrimmedLength() const;

        /** How long the segment occupies on the montage timeline. */
        float GetTimelineLength() const;

        float GetEndTime() const { return StartTime + GetTimelineLength(); }

        /** In-clip time this segment samples at for a montage-timeline position inside it. */
        float ResolveClipTime(float MontageTime) const;
    };

    // A named lane of segments; an anim graph Slot node of the same name plays whatever it produces.
    REFLECT()
    struct RUNTIME_API SAnimMontageSlotTrack
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Slot")
        FName SlotName = "DefaultSlot";

        PROPERTY(Editable, Category = "Slot")
        TVector<SAnimMontageSegment> Segments;

        /** Timeline end of the last segment. */
        float GetEndTime() const;

        /** Segment covering MontageTime, or INDEX_NONE when the time falls in a gap. */
        int32 FindSegment(float MontageTime) const;
    };

    // A named region running to the next section in time order, then continuing into NextSection.
    REFLECT()
    struct RUNTIME_API SAnimMontageSection
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Section")
        FName Name = "Default";

        PROPERTY(Editable, Category = "Section", Units = "s", ClampMin = 0.0f)
        float StartTime = 0.0f;

        /** Section entered when this one ends. None ends the montage. */
        PROPERTY(Editable, Category = "Section")
        FName NextSection;
    };

    // Point event fired once as the playhead crosses it, weighted by the montage's blend weight.
    REFLECT()
    struct RUNTIME_API SAnimMontageNotify
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Notify")
        FName Name;

        PROPERTY(Editable, Category = "Notify", Units = "s", ClampMin = 0.0f)
        float Time = 0.0f;

        /** Display lane in the montage editor, carried through to the fired event. */
        PROPERTY(Editable, Category = "Notify")
        FName Track = "Notifies";

        /** Pick a notify type to run its own code; leave empty for a name-only event. */
        PROPERTY(Editable, Category = "Notify")
        TInstancedStruct<SAnimNotify> Notify;
    };

    // Windowed event: Begin on entry, Tick while inside, End on exit or when the montage stops.
    REFLECT()
    struct RUNTIME_API SAnimMontageNotifyState
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Notify")
        FName Name;

        PROPERTY(Editable, Category = "Notify", Units = "s", ClampMin = 0.0f)
        float StartTime = 0.0f;

        PROPERTY(Editable, Category = "Notify", Units = "s", ClampMin = 0.0f)
        float EndTime = 0.0f;

        PROPERTY(Editable, Category = "Notify")
        FName Track = "Notifies";

        /** Pick a notify type to run its own code; leave empty for a name-only event. */
        PROPERTY(Editable, Category = "Notify")
        TInstancedStruct<SAnimNotifyState> Notify;
    };

    // Which clip a slot samples at a given montage position, and how far into it.
    struct FAnimMontageSlotSample
    {
        CAnimation* Clip = nullptr;
        float ClipTime = 0.0f;
        float PrevClipTime = 0.0f;
        bool bLooping = false;
    };

    // Gameplay-driven animation on named slots; the graph decides where a slot sits in the blend.
    REFLECT()
    class RUNTIME_API CAnimationMontage : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }
        void PostLoad() override;

        /** Skeleton every segment clip is authored against. */
        PROPERTY(Editable, Category = "Montage")
        TObjectPtr<CSkeleton> Skeleton;

        PROPERTY(Editable, Category = "Montage")
        TVector<SAnimMontageSlotTrack> SlotTracks;

        /** Named regions in timeline order; the first one is where playback starts by default. */
        PROPERTY(Editable, Category = "Sections")
        TVector<SAnimMontageSection> Sections;

        /** Seconds the montage takes to reach full weight on its slots. */
        PROPERTY(Editable, Category = "Blending", Units = "s", ClampMin = 0.0f)
        float BlendInTime = 0.15f;

        /** Seconds the montage takes to fade back out to the graph pose. */
        PROPERTY(Editable, Category = "Blending", Units = "s", ClampMin = 0.0f)
        float BlendOutTime = 0.2f;

        /** Seconds before the end the blend out begins; negative lands the fade on the last frame. */
        PROPERTY(Editable, Category = "Blending", Units = "s")
        float BlendOutTriggerTime = -1.0f;

        /** Extract root motion from segment clips that enable it and drive the entity with it. */
        PROPERTY(Editable, Category = "Root Motion")
        bool bEnableRootMotion = false;

        PROPERTY(Editable, Category = "Notifies")
        TVector<SAnimMontageNotify> Notifies;

        PROPERTY(Editable, Category = "Notifies")
        TVector<SAnimMontageNotifyState> NotifyStates;

        /** Notify lanes in display order; persisted so empty lanes and ordering survive a reload. */
        PROPERTY(Editable, Category = "Notifies")
        TVector<FName> NotifyTracks;

        /** Longest slot track's end time. */
        float GetDuration() const;

        int32 FindSlotIndex(const FName& SlotName) const;

        int32 FindSectionIndex(const FName& SectionName) const;

        /** Section covering MontageTime; INDEX_NONE when the montage has no sections. */
        int32 FindSectionAtTime(float MontageTime) const;

        /** Where a section stops: the next section's start, or the montage end. */
        float GetSectionEndTime(int32 SectionIndex) const;

        /** Fills OutSample for the named slot; false when the slot has no segment at that time. */
        bool EvaluateSlot(const FName& SlotName, float MontageTime, float PrevMontageTime, FAnimMontageSlotSample& OutSample) const;

        /** Every slot name this montage carries a track for. */
        void GetSlotNames(TVector<FName>& OutNames) const;

        /** Blend-out start for a section, honouring BlendOutTriggerTime. */
        float GetBlendOutStartTime(int32 SectionIndex) const;

        /** Adds any notify lane referenced by a notify but missing from NotifyTracks. */
        void EnsureNotifyTracks();
    };
}
