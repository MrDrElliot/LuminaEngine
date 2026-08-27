#pragma once

#include "World/ECS/Registry.h"


#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Sequence.generated.h"

namespace Lumina
{
    class CPrefab;
    class CSequence;
    class CWorld;

    REFLECT()
    enum class ESequenceBindingKind : uint8
    {
        // Resolve an entity that already exists in the world, by name.
        Possess,
        // Spawn one from a prefab for the sequence's lifetime, then destroy it. Makes a cutscene portable
        // between levels because it carries its own cast.
        Spawn,
    };

    REFLECT()
    struct RUNTIME_API SSequenceBinding
    {
        GENERATED_BODY()

        /** Label in the sequencer, and the entity name a Possess binding resolves against. */
        PROPERTY(Editable, Category = "Binding")
        FName Name;

        PROPERTY(Editable, Category = "Binding")
        ESequenceBindingKind Kind = ESequenceBindingKind::Possess;

        PROPERTY(Editable, Category = "Binding")
        TObjectPtr<CPrefab> SpawnPrefab;
    };

    // What a track is handed when it evaluates. Bindings are resolved once per play rather than per track,
    // so a shot with twenty tracks on one actor does one lookup.
    struct FSequenceEvalContext
    {
        CWorld*                      World = nullptr;
        const CSequence*             Sequence = nullptr;

        float                        Time = 0.0f;
        float                        PreviousTime = 0.0f;

        // Indexed by binding index; ECS::NullEntity where a binding did not resolve.
        const TVector<ECS::FEntity>* BoundEntities = nullptr;

        // True on the frame playback started or jumped, so tracks that latch state (camera cuts) can
        // re-apply rather than assuming continuity.
        bool                         bJumped = false;

        ECS::FEntity Resolve(int32 BindingIndex) const;
    };

    // Base for everything that drives something over time. A new track type is a new subclass with an
    // Evaluate override; nothing in the sequence, player, or editor needs to know about it.
    REFLECT()
    class RUNTIME_API CSequenceTrack : public CObject
    {
        GENERATED_BODY()

    public:

        /** Binding this track drives. INDEX_NONE for tracks that act on the sequence itself. */
        PROPERTY(Editable, Category = "Track")
        int32 BindingIndex = INDEX_NONE;

        PROPERTY(Editable, Category = "Track")
        bool bEnabled = true;

        /** Row label in the sequencer. */
        virtual FStringView GetTrackDisplayName() const { return "Track"; }

        /** Color of the track's row, so kinds are distinguishable at a glance. */
        virtual FVector4 GetTrackColor() const { return FVector4(0.45f, 0.55f, 0.70f, 1.0f); }

        /** Writes this track's contribution for Context.Time. Called every evaluated frame. */
        virtual void Evaluate(const FSequenceEvalContext& Context) const {}
    };

    // Three curves treated as one channel group, so a track stores Location/Rotation/Scale rather than
    // nine loose curves.
    REFLECT()
    struct RUNTIME_API SSequenceVectorCurve
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Curve")
        SCurve X;

        PROPERTY(Editable, Category = "Curve")
        SCurve Y;

        PROPERTY(Editable, Category = "Curve")
        SCurve Z;

        /** Off leaves the channel alone entirely, which is what lets a track animate rotation without
         *  fighting gameplay for position. */
        PROPERTY(Editable, Category = "Curve")
        bool bEnabled = false;

        FVector3 Evaluate(float Time) const;
    };

    REFLECT()
    class RUNTIME_API CSequenceTrack_Transform : public CSequenceTrack
    {
        GENERATED_BODY()

    public:

        FStringView GetTrackDisplayName() const override { return "Transform"; }
        FVector4 GetTrackColor() const override { return FVector4(0.40f, 0.65f, 1.00f, 1.0f); }

        void Evaluate(const FSequenceEvalContext& Context) const override;

        PROPERTY(Editable, Category = "Transform")
        SSequenceVectorCurve Location;

        /** Euler degrees. */
        PROPERTY(Editable, Category = "Transform")
        SSequenceVectorCurve Rotation;

        PROPERTY(Editable, Category = "Transform")
        SSequenceVectorCurve Scale;
    };

    REFLECT()
    struct RUNTIME_API SSequenceCameraCut
    {
        GENERATED_BODY()

        /** Binding holding the camera this cut switches to. */
        PROPERTY(Editable, Category = "Cut")
        int32 BindingIndex = INDEX_NONE;

        PROPERTY(Editable, Category = "Cut")
        float StartTime = 0.0f;

        PROPERTY(Editable, Category = "Cut")
        float EndTime = 1.0f;
    };

    // Which bound camera is live over each range. One per sequence in practice; cuts are ordered and the
    // first containing the playhead wins, so overlaps resolve predictably rather than fighting.
    REFLECT()
    class RUNTIME_API CSequenceTrack_CameraCut : public CSequenceTrack
    {
        GENERATED_BODY()

    public:

        FStringView GetTrackDisplayName() const override { return "Camera Cuts"; }
        FVector4 GetTrackColor() const override { return FVector4(1.00f, 0.70f, 0.30f, 1.0f); }

        void Evaluate(const FSequenceEvalContext& Context) const override;

        PROPERTY(Editable, Category = "Camera")
        TVector<SSequenceCameraCut> Cuts;

        /** Cut covering Time, or INDEX_NONE. */
        int32 FindCutAt(float Time) const;
    };

    // Live binding state for one sequence being played. Held by the player component and by the editor's
    // sequencer mode, so authoring and playback resolve, restore and clean up through identical code.
    struct RUNTIME_API FSequenceInstance
    {
        struct FRestoreEntry
        {
            ECS::FEntity Entity = ECS::NullEntity;
            FVector3     Location;
            FVector3     Rotation;
            FVector3     Scale = FVector3(1.0f);
        };

        // Indexed by binding index; ECS::NullEntity where a binding did not resolve.
        TVector<ECS::FEntity>  BoundEntities;
        TVector<ECS::FEntity>  SpawnedEntities;
        TVector<FRestoreEntry> RestoreState;

        bool bBound = false;

        /** Resolves every binding, spawning prefab-backed ones and snapshotting possessed transforms. */
        void Bind(const CSequence* Sequence, CWorld* World);

        /** Destroys spawned entities and, when bRestore, puts possessed ones back where they started.
         *  Without the restore a sequence permanently displaces whatever it drove. */
        void Release(CWorld* World, bool bRestore);

        void Evaluate(const CSequence* Sequence, CWorld* World, float Time, float PreviousTime, bool bJumped);
    };

    REFLECT()
    class RUNTIME_API CSequence : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        PROPERTY(Editable, ClampMin = 0.0f, Category = "Sequence")
        float Duration = 5.0f;

        /** Display and snapping rate. Evaluation is continuous, so this never quantizes playback itself. */
        PROPERTY(Editable, ClampMin = 1, ClampMax = 240, Category = "Sequence")
        int32 FrameRate = 30;

        PROPERTY(Editable, Category = "Sequence")
        TVector<SSequenceBinding> Bindings;

        PROPERTY(Editable, Category = "Sequence")
        TVector<TObjectPtr<CSequenceTrack>> Tracks;

        int32 GetFrameCount() const;
        float FrameToTime(int32 Frame) const;
        int32 TimeToFrame(float Time) const;
        float SnapToFrame(float Time) const;

        /** Runs every enabled track. The player and the editor's scrub both go through here, so what is
         *  authored and what ships are evaluated by the same code. */
        void Evaluate(const FSequenceEvalContext& Context) const;
    };
}
