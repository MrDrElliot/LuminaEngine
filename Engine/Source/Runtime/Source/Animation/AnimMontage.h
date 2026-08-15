#pragma once

#include "Animation/AnimEvents.h"
#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectHandleTyped.h"

namespace Lumina
{
    class CAnimation;

    enum class EAnimMontageState : uint8
    {
        BlendingIn,
        Playing,
        BlendingOut,
        Finished,
    };

    // One playing montage. Position is montage-timeline seconds; Weight is what the slot blends at.
    struct RUNTIME_API FAnimMontageInstance
    {
        TObjectPtr<CAnimationMontage> Montage;

        /** Identifies this play through the montage; reusing the asset later gets a fresh id. */
        uint32 InstanceID = 0;

        float Position = 0.0f;
        float PrevPosition = 0.0f;
        float PlayRate = 1.0f;

        float Weight = 0.0f;
        float BlendRate = 0.0f;

        EAnimMontageState State = EAnimMontageState::BlendingIn;

        int32 CurrentSection = INDEX_NONE;

        /** Section the playhead jumps to at the end of the current one, overriding the authored link. */
        FName QueuedSection;

        /** Indices into the montage's NotifyStates whose window the playhead was inside last update. */
        TVector<int32> ActiveNotifyStates;

        bool IsActive() const { return State != EAnimMontageState::Finished; }
    };

    // What one montage contributes to a slot this frame, already resolved to a clip and a weight.
    struct FAnimMontageSlotContribution
    {
        const CAnimationMontage* Montage = nullptr;
        CAnimation* Clip = nullptr;
        float ClipTime = 0.0f;
        float PrevClipTime = 0.0f;
        float Weight = 0.0f;
        bool bLooping = false;
        bool bRootMotion = false;
    };

    // Owns every montage playing on one entity. Advances update-side; the graph's Slot opcode reads it.
    struct RUNTIME_API FAnimMontagePlayer
    {
        TVector<FAnimMontageInstance> Instances;

        /** Starts a montage, blending out anything already playing on a slot it uses. Returns its id. */
        uint32 Play(CAnimationMontage* Montage, float PlayRate = 1.0f, const FName& StartSection = FName());

        /** Blends the montage out. A negative BlendOutTime uses the montage's own. */
        void Stop(const CAnimationMontage* Montage, float BlendOutTime = -1.0f);

        void StopAll(float BlendOutTime = -1.0f);

        /** Drops every instance without blending; used when the graph goes away. */
        void Reset();

        /** Moves the playhead to the named section's start. False when the section does not exist. */
        bool JumpToSection(const CAnimationMontage* Montage, const FName& SectionName);

        /** Overrides which section follows the one currently playing, for combo chaining. */
        bool SetNextSection(const CAnimationMontage* Montage, const FName& SectionName);

        bool IsPlaying(const CAnimationMontage* Montage) const;

        /** True while any montage is contributing weight. */
        bool HasActive() const;

        FName GetCurrentSection(const CAnimationMontage* Montage) const;
        float GetPosition(const CAnimationMontage* Montage) const;
        float GetWeight(const CAnimationMontage* Montage) const;
        void  SetPlayRate(const CAnimationMontage* Montage, float PlayRate);

        /** Advances every instance, fires notifies into OutEvents, and reaps finished instances. */
        void Update(float DeltaTime, TVector<FAnimNotifyEvent>* OutEvents);

        /** Contributions for one slot, oldest first, so later instances layer on top. */
        void GatherSlot(const FName& SlotName, TVector<FAnimMontageSlotContribution>& OutContributions) const;

    private:

        FAnimMontageInstance* Find(const CAnimationMontage* Montage);
        const FAnimMontageInstance* Find(const CAnimationMontage* Montage) const;

        void BeginBlendOut(FAnimMontageInstance& Instance, float BlendOutTime);
        void AdvanceInstance(FAnimMontageInstance& Instance, float DeltaTime, TVector<FAnimNotifyEvent>* OutEvents);
        void DispatchNotifies(FAnimMontageInstance& Instance, bool bAdvanced, TVector<FAnimNotifyEvent>* OutEvents);
        void EndActiveNotifyStates(FAnimMontageInstance& Instance, TVector<FAnimNotifyEvent>* OutEvents);

        uint32 NextInstanceID = 1;
    };
}
