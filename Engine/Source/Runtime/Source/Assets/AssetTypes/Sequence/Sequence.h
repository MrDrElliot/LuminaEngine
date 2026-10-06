#pragma once

#include "World/ECS/Registry.h"


#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Audio/AudioTypes.h"
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
    class CSoundBase;
    class CWorld;
    struct FSequenceInstance;

    REFLECT()
    enum class ESequenceBindingKind : uint8
    {
        // Resolve an entity that already exists in the world, by name.
        Possess,
        // Spawn one from a prefab for the sequence's lifetime, then destroy it. Makes a cutscene portable
        // between levels because it carries its own cast.
        Spawn,
        // Spawn a bare camera named after the binding for the sequence's lifetime, the usual subject of camera cuts.
        Camera,
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
        TStrongObjectPtr<CPrefab> SpawnPrefab;
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

        // The first frame after binding, when keys sitting exactly on the start time still fire.
        bool                         bStarted = false;

        // A loop came round this frame, so the interval runs from PreviousTime to the end and on from zero.
        bool                         bWrapped = false;

        // Held at Time rather than advancing, so anything with its own clock such as sound waits too.
        bool                         bPaused = false;

        // Mutable per-play state, for tracks that hold handles or report events.
        FSequenceInstance*           Instance = nullptr;

        ECS::FEntity Resolve(int32 BindingIndex) const;

        // Whether playback passed KeyTime this frame, which is when a one-shot key such as an event fires.
        bool Crossed(float KeyTime) const;
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

        // Lower evaluates first, so tracks that read what others wrote, such as a look-at or a cut, go later.
        virtual int32 GetEvaluationOrder() const { return 0; }

        // Undoes anything the track left on the world when playback stops.
        virtual void OnRelease(FSequenceInstance& Instance, CWorld* World) const {}
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

        // Moves each key by whole turns to within half a turn of the key before it, so rotations take the short way round.
        void UnwindAngles();
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

        // Pitch, yaw and roll in degrees, applied yaw first so any heading keys and interpolates without flipping.
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

        // Seconds spent easing in from the previous view, where zero is a hard cut.
        PROPERTY(Editable, Category = "Cut", ClampMin = 0.0f)
        float BlendTime = 0.0f;
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
        int32 GetEvaluationOrder() const override { return 100; }

        PROPERTY(Editable, Category = "Camera")
        TVector<SSequenceCameraCut> Cuts;

        /** Cut covering Time, or INDEX_NONE. */
        int32 FindCutAt(float Time) const;
    };

    REFLECT()
    struct RUNTIME_API SSequenceEventKey
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Event")
        float Time = 0.0f;

        PROPERTY(Editable, Category = "Event")
        FName Name;

        // Free text for whatever handles the event, such as a target name or an amount.
        PROPERTY(Editable, Category = "Event")
        FString Payload;
    };

    // An event that fired, handed to script through CSequenceLibrary::ConsumeEvents.
    REFLECT()
    struct RUNTIME_API SSequenceFiredEvent
    {
        GENERATED_BODY()

        PROPERTY()
        FName Name;

        PROPERTY()
        FString Payload;

        PROPERTY()
        float Time = 0.0f;

        // Entity the event track is bound to, or the null entity for a sequence-wide track.
        PROPERTY()
        ECS::FEntity Target = ECS::NullEntity;
    };

    // Named moments that gameplay reacts to, such as an explosion going off on a beat of the edit.
    REFLECT()
    class RUNTIME_API CSequenceTrack_Event : public CSequenceTrack
    {
        GENERATED_BODY()

    public:

        FStringView GetTrackDisplayName() const override { return "Events"; }
        FVector4 GetTrackColor() const override { return FVector4(0.95f, 0.45f, 0.45f, 1.0f); }

        void Evaluate(const FSequenceEvalContext& Context) const override;
        int32 GetEvaluationOrder() const override { return 90; }

        PROPERTY(Editable, Category = "Events")
        TVector<SSequenceEventKey> Keys;
    };

    // One reflected number on one component of the bound entity, driven by a curve.
    REFLECT()
    class RUNTIME_API CSequenceTrack_Property : public CSequenceTrack
    {
        GENERATED_BODY()

    public:

        FStringView GetTrackDisplayName() const override { return "Property"; }
        FVector4 GetTrackColor() const override { return FVector4(0.60f, 0.85f, 0.50f, 1.0f); }

        void Evaluate(const FSequenceEvalContext& Context) const override;
        int32 GetEvaluationOrder() const override { return 10; }

        // Reflected component type name, such as SCameraComponent or SPointLightComponent.
        PROPERTY(Editable, Category = "Property")
        FName ComponentType;

        // Dotted path from the component to a float, int or bool, such as FOV or PostProcess.DepthOfFieldFocusDistance.
        PROPERTY(Editable, Category = "Property")
        FString PropertyPath;

        PROPERTY(Editable, Category = "Property")
        SCurve Curve;

        // Reads the value the property holds on Entity right now, for keying what the editor shows.
        bool SampleValue(CWorld* World, ECS::FEntity Entity, float& OutValue) const;
    };

    REFLECT()
    struct RUNTIME_API SSequenceAudioClip
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Audio")
        float StartTime = 0.0f;

        PROPERTY(Editable, Category = "Audio")
        TStrongObjectPtr<CSoundBase> Sound;

        PROPERTY(Editable, Category = "Audio", ClampMin = 0.0f)
        float Volume = 1.0f;

        PROPERTY(Editable, Category = "Audio", ClampMin = 0.01f)
        float Pitch = 1.0f;

        // Music for a score, so the player's music volume governs it; effects otherwise.
        PROPERTY(Editable, Category = "Audio")
        EAudioBus Bus = EAudioBus::SFX;
    };

    // Sounds that follow the playhead, starting part way in when playback begins inside one and holding while the sequence is paused.
    REFLECT()
    class RUNTIME_API CSequenceTrack_Audio : public CSequenceTrack
    {
        GENERATED_BODY()

    public:

        FStringView GetTrackDisplayName() const override { return "Audio"; }
        FVector4 GetTrackColor() const override { return FVector4(0.35f, 0.80f, 0.75f, 1.0f); }

        void Evaluate(const FSequenceEvalContext& Context) const override;
        void OnRelease(FSequenceInstance& Instance, CWorld* World) const override;
        int32 GetEvaluationOrder() const override { return 80; }

        PROPERTY(Editable, Category = "Audio")
        TVector<SSequenceAudioClip> Clips;
    };

    // A fade over the whole picture that holds across camera cuts, for opening, closing and cutting to black.
    REFLECT()
    class RUNTIME_API CSequenceTrack_Fade : public CSequenceTrack
    {
        GENERATED_BODY()

    public:

        FStringView GetTrackDisplayName() const override { return "Fade"; }
        FVector4 GetTrackColor() const override { return FVector4(0.55f, 0.55f, 0.55f, 1.0f); }

        void Evaluate(const FSequenceEvalContext& Context) const override;
        int32 GetEvaluationOrder() const override { return 20; }

        // Zero shows the picture and one covers it in Color.
        PROPERTY(Editable, Category = "Fade")
        SCurve Amount;

        PROPERTY(Editable, Color, Category = "Fade")
        FVector3 Color = FVector3(0.0f);
    };

    // World time scale over the shot, for slow motion; the sequence itself keeps real time so its edit holds.
    REFLECT()
    class RUNTIME_API CSequenceTrack_TimeDilation : public CSequenceTrack
    {
        GENERATED_BODY()

    public:

        FStringView GetTrackDisplayName() const override { return "Time Dilation"; }
        FVector4 GetTrackColor() const override { return FVector4(0.85f, 0.55f, 0.95f, 1.0f); }

        void Evaluate(const FSequenceEvalContext& Context) const override;
        void OnRelease(FSequenceInstance& Instance, CWorld* World) const override;
        int32 GetEvaluationOrder() const override { return 5; }

        PROPERTY(Editable, Category = "Time")
        SCurve Scale;
    };

    // Turns the bound entity, usually a camera, to face another binding every frame after it has been moved.
    REFLECT()
    class RUNTIME_API CSequenceTrack_LookAt : public CSequenceTrack
    {
        GENERATED_BODY()

    public:

        FStringView GetTrackDisplayName() const override { return "Look At"; }
        FVector4 GetTrackColor() const override { return FVector4(1.00f, 0.85f, 0.40f, 1.0f); }

        void Evaluate(const FSequenceEvalContext& Context) const override;
        int32 GetEvaluationOrder() const override { return 50; }

        // Binding to follow, or none to aim at TargetOffset as a fixed point in the world.
        PROPERTY(Editable, Category = "Look At")
        int32 TargetBindingIndex = INDEX_NONE;

        // Added to the target's position, so a shot can frame a head rather than the feet.
        PROPERTY(Editable, Category = "Look At")
        FVector3 TargetOffset = FVector3(0.0f);

        // How much of the aim to apply, so a key can ease a shot from free rotation onto the target.
        PROPERTY(Editable, Category = "Look At")
        SCurve Weight;

        // Keeps a bound camera's depth of field focused on the target, the way a focus puller follows an actor.
        PROPERTY(Editable, Category = "Look At")
        bool bAutoFocus = false;
    };

    // A looping shake over the live view, such as a handheld drift, scaled over time by Intensity.
    REFLECT()
    class RUNTIME_API CSequenceTrack_CameraShake : public CSequenceTrack
    {
        GENERATED_BODY()

    public:

        FStringView GetTrackDisplayName() const override { return "Camera Shake"; }
        FVector4 GetTrackColor() const override { return FVector4(0.95f, 0.60f, 0.75f, 1.0f); }

        void Evaluate(const FSequenceEvalContext& Context) const override;
        void OnRelease(FSequenceInstance& Instance, CWorld* World) const override;
        int32 GetEvaluationOrder() const override { return 60; }

        // Largest offset along the view's own axes, in world units.
        PROPERTY(Editable, Category = "Shake")
        FVector3 LocationAmplitude = FVector3(0.02f, 0.02f, 0.0f);

        // Largest pitch, yaw and roll, in degrees.
        PROPERTY(Editable, Category = "Shake")
        FVector3 RotationAmplitude = FVector3(0.4f, 0.5f, 0.3f);

        PROPERTY(Editable, Category = "Shake", ClampMin = 0.01f)
        float Frequency = 1.2f;

        // Scales both amplitudes, where an unkeyed curve means full strength throughout.
        PROPERTY(Editable, Category = "Shake")
        SCurve Intensity;
    };

    // Live binding state for one sequence being played. Held by the player component and by the editor's
    // sequencer mode, so authoring and playback resolve, restore and clean up through identical code.
    struct RUNTIME_API FSequenceInstance
    {
        struct FRestoreEntry
        {
            ECS::FEntity Entity = ECS::NullEntity;
            FVector3     Location;
            FQuat        Rotation;
            FVector3     Scale = FVector3(1.0f);
        };

        // Indexed by binding index; ECS::NullEntity where a binding did not resolve.
        struct FPlayingSound
        {
            const CSequenceTrack* Track = nullptr;
            FAudioHandle          Handle;
            int32                 ClipIndex = INDEX_NONE;
            bool                  bPaused = false;
        };

        TVector<ECS::FEntity>  BoundEntities;
        TVector<ECS::FEntity>  SpawnedEntities;
        TVector<FRestoreEntry> RestoreState;

        // Events fired since script last consumed them.
        TVector<SSequenceFiredEvent> FiredEvents;
        TVector<FPlayingSound>       Sounds;

        struct FPlayingShake
        {
            const CSequenceTrack* Track = nullptr;
            uint32                Handle = 0;
        };
        TVector<FPlayingShake>       Shakes;

        // What the sequence displaced, put back on release.
        ECS::FEntity PreviousCamera = ECS::NullEntity;
        float        PreviousTimeDilation = 1.0f;

        // Written by fade tracks each evaluation and handed to the camera system after them.
        float        OverlayFade = 0.0f;
        FVector3     OverlayFadeColor = FVector3(0.0f);

        const CSequence* BoundSequence = nullptr;
        bool bBound = false;

        // Off keeps camera cuts from taking the view, so an editor can fly its own camera through a shot.
        bool bDriveCamera = true;

        /** Resolves every binding, spawning prefab-backed ones and snapshotting possessed transforms. */
        void Bind(const CSequence* Sequence, CWorld* World);

        /** Destroys spawned entities and, when bRestore, puts possessed ones back where they started.
         *  Without the restore a sequence permanently displaces whatever it drove. */
        void Release(CWorld* World, bool bRestore);

        void Evaluate(const CSequence* Sequence, CWorld* World, float Time, float PreviousTime, bool bJumped, bool bStarted = false, bool bWrapped = false,
                      bool bPaused = false);
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
        TVector<TStrongObjectPtr<CSequenceTrack>> Tracks;

        // Black bars for the whole sequence as the picture's width over height, where zero shows none.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 4.0f, Category = "Sequence")
        float LetterboxAspect = 0.0f;

        int32 GetFrameCount() const;
        float FrameToTime(int32 Frame) const;
        int32 TimeToFrame(float Time) const;
        float SnapToFrame(float Time) const;

        /** Runs every enabled track. The player and the editor's scrub both go through here, so what is
         *  authored and what ships are evaluated by the same code. */
        void Evaluate(const FSequenceEvalContext& Context) const;
    };
}
