#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPSequenceTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SSequenceRefParams
    {
        GENERATED_BODY()

        // GUID or path of the sequence asset.
        PROPERTY()
        FString Sequence;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateSequenceParams
    {
        GENERATED_BODY()

        // Folder to create it in, such as /Game/Content/Cinematics.
        PROPERTY()
        FString Folder;

        PROPERTY()
        FString Name;

        // Length in seconds. Adding a shot past the end lengthens it.
        PROPERTY()
        float Duration = 10.0f;

        PROPERTY()
        int32 FrameRate = 30;

        // Width over height between black bars for the whole sequence, such as 2.39. Zero shows none.
        PROPERTY()
        float LetterboxAspect = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateSequenceResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Guid;

        PROPERTY()
        FString Path;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceSettingsParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        // Negative leaves it alone.
        PROPERTY()
        float Duration = -1.0f;

        // Zero leaves it alone.
        PROPERTY()
        int32 FrameRate = 0;

        // Negative leaves it alone, zero removes the bars.
        PROPERTY()
        float LetterboxAspect = -1.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceBindingInfo
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Index = 0;

        PROPERTY()
        FString Name;

        // Possess, Spawn or Camera.
        PROPERTY()
        FString Kind;

        // GUID of the prefab a Spawn binding creates.
        PROPERTY()
        FString Prefab;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceTrackInfo
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Index = 0;

        PROPERTY()
        FString Type;

        // Name of the binding it drives, empty for a sequence-wide track.
        PROPERTY()
        FString Binding;

        PROPERTY()
        bool bEnabled = true;

        // Every field of the track as JSON, keys included unless the describe asked for a summary.
        PROPERTY()
        FString Values;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeSequenceParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        // Report each curve as its key count rather than every key, which keeps a long sequence readable.
        PROPERTY()
        bool bSummary = false;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeSequenceResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        float Duration = 0.0f;

        PROPERTY()
        int32 FrameRate = 30;

        PROPERTY()
        float LetterboxAspect = 0.0f;

        PROPERTY()
        TVector<SSequenceBindingInfo> Bindings;

        PROPERTY()
        TVector<SSequenceTrackInfo> Tracks;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceTrackTypeInfo
    {
        GENERATED_BODY()

        // Name to pass to sequence.add_track.
        PROPERTY()
        FString Name;

        PROPERTY()
        FString Description;

        // Whether it needs a binding to drive, as opposed to acting on the whole sequence.
        PROPERTY()
        bool bNeedsBinding = false;

        // JSON Schema of the fields sequence.set_track_property and add_track's Settings accept.
        PROPERTY()
        FString Schema;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceTrackTypesParams
    {
        GENERATED_BODY()

        PROPERTY()
        bool bIncludeSchema = false;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceTrackTypesResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SSequenceTrackTypeInfo> Types;
    };

    REFLECT()
    struct MCPEDITOR_API SAddSequenceBindingParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        // For Possess, the name of the entity in the world to drive.
        PROPERTY()
        FString Name;

        // Possess an existing entity, Spawn one from a prefab for the sequence's length, or Camera for a bare cinematic camera.
        PROPERTY()
        FString Kind = "Possess";

        // GUID of the prefab a Spawn binding creates.
        PROPERTY()
        FString Prefab;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceIndexResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Index = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SRemoveSequenceBindingParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        // Name or index. Its tracks and cuts go with it.
        PROPERTY()
        FString Binding;
    };

    REFLECT()
    struct MCPEDITOR_API SAddSequenceTrackParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        // From sequence.track_types, such as Transform, Property, Fade or LookAt.
        PROPERTY()
        FString Type;

        // Name or index of the binding to drive. Empty for a sequence-wide track such as Fade or Events.
        PROPERTY()
        FString Binding;

        // Optional initial fields as a JSON object keyed by field name.
        PROPERTY(RawJson)
        FString Settings;
    };

    REFLECT()
    struct MCPEDITOR_API SRemoveSequenceTrackParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        // Negative removes every track.
        PROPERTY()
        int32 Track = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SSetSequenceTrackPropertyParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        PROPERTY()
        int32 Track = 0;

        // Field path on the track, such as bEnabled, TargetOffset, Color or Clips[0].Volume.
        PROPERTY()
        FString Path;

        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SSequencePropertyResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Previous;

        PROPERTY()
        FString Current;
    };

    REFLECT()
    struct MCPEDITOR_API SSetSequenceKeysParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        PROPERTY()
        int32 Track = 0;

        // The curve to key, such as Location, Rotation, Scale, Location.Z, Curve, Amount, Weight or Intensity.
        PROPERTY()
        FString Channel;

        // Array of keys each with Time and Value, a number or for a vector channel an [x,y,z] array, and optionally its own Interp.
        PROPERTY(RawJson)
        FString Keys;

        // Cubic for smooth motion, Linear, or Constant to hold until the next key.
        PROPERTY()
        FString Interp = "Cubic";

        // Clear the channel's existing keys first.
        PROPERTY()
        bool bReplace = true;
    };

    REFLECT()
    struct MCPEDITOR_API SSetSequenceKeysResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 KeyCount = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SAddSequenceCameraCutParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        // Name or index of the camera binding.
        PROPERTY()
        FString Camera;

        PROPERTY()
        float Start = 0.0f;

        PROPERTY()
        float End = 1.0f;

        // Seconds easing in from the previous shot, where zero is a hard cut.
        PROPERTY()
        float BlendTime = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SAddSequenceEventParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        PROPERTY()
        float Time = 0.0f;

        // What gameplay listens for through CSequenceLibrary.ConsumeEvents.
        PROPERTY()
        FString Name;

        PROPERTY()
        FString Payload;

        // Name or index of the binding the event is about, empty for none.
        PROPERTY()
        FString Binding;
    };

    REFLECT()
    struct MCPEDITOR_API SAddSequenceAudioParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        // GUID or path of the sound.
        PROPERTY()
        FString Sound;

        PROPERTY()
        float Start = 0.0f;

        PROPERTY()
        float Volume = 1.0f;

        PROPERTY()
        float Pitch = 1.0f;

        // Name or index of the binding to play it at, empty for stereo music.
        PROPERTY()
        FString Binding;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceShotParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        // Camera binding for the shot, created as a Camera binding when missing. Empty names a new one after the shot count.
        PROPERTY()
        FString Camera;

        PROPERTY()
        float Start = 0.0f;

        PROPERTY()
        float End = 3.0f;

        // Array of camera keys, each with Time relative to Start and Location, plus any of Rotation, Target, FOV, Focus, FStop and Ground.
        PROPERTY(RawJson)
        FString Keys;

        // Binding the camera follows with its aim for the whole shot, overriding key rotations.
        PROPERTY()
        FString LookAt;

        // Fixed world point to aim at when there is no LookAt binding, as [x,y,z].
        PROPERTY(RawJson)
        FString LookAtPoint;

        // Added to the LookAt binding's position, so a shot frames a chest rather than the feet.
        PROPERTY(RawJson)
        FString LookAtOffset;

        // Vertical field of view in degrees for the whole shot, unless keys say otherwise. Zero keeps the camera's.
        PROPERTY()
        float FOV = 0.0f;

        // Aperture for depth of field, such as 2.8. Zero leaves it off.
        PROPERTY()
        float FStop = 0.0f;

        // With a look-at and an aperture, keep focus on the subject.
        PROPERTY()
        bool bAutoFocus = true;

        // Strength of a handheld drift, where 1 reads as a steady operator and 3 as running. Zero is a locked-off camera.
        PROPERTY()
        float Handheld = 0.0f;

        // Seconds easing in from the previous shot, where zero is a hard cut.
        PROPERTY()
        float BlendTime = 0.0f;

        // Add the camera cut that makes the shot live. Off only builds the camera.
        PROPERTY()
        bool bCut = true;

        PROPERTY()
        FString Interp = "Cubic";

        // Read every height in the shot, keys, targets and LookAtPoint, as above the terrain. A key's own Ground overrides it.
        PROPERTY()
        bool bGround = false;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceShotResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Camera;

        PROPERTY()
        int32 CameraBinding = 0;

        // Indices of the tracks the shot created or keyed.
        PROPERTY()
        TVector<int32> Tracks;
    };

    REFLECT()
    struct MCPEDITOR_API SSequencePlayParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        PROPERTY()
        bool bLoop = false;

        PROPERTY()
        float PlayRate = 1.0f;

        PROPERTY()
        float StartTime = 0.0f;

        // Bind and show StartTime without advancing, for framing a shot.
        PROPERTY()
        bool bPaused = false;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceControlParams
    {
        GENERATED_BODY()

        // Pause, Resume, Stop, Seek or Restart.
        PROPERTY()
        FString Action;

        // Seconds, for Seek.
        PROPERTY()
        float Time = 0.0f;

        // Which sequence's player, empty for every player in the world.
        PROPERTY()
        FString Sequence;
    };

    REFLECT()
    struct MCPEDITOR_API SSequencePlayerInfo
    {
        GENERATED_BODY()

        // Token for the player entity.
        PROPERTY()
        FString Player;

        PROPERTY()
        FString Sequence;

        PROPERTY()
        float Time = 0.0f;

        PROPERTY()
        float Duration = 0.0f;

        PROPERTY()
        bool bPlaying = false;

        PROPERTY()
        bool bPaused = false;

        // Binding of the camera the cut track has live at the playhead.
        PROPERTY()
        FString LiveCamera;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceStateResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SSequencePlayerInfo> Players;
    };

    REFLECT()
    struct MCPEDITOR_API SMovieRenderParams
    {
        GENERATED_BODY()

        // Sequence to play from its start for its whole length. Empty records whatever the game shows for Seconds.
        PROPERTY()
        FString Sequence;

        // MP4 to write. Empty with no PngDirectory writes to Saved/Movies under the engine.
        PROPERTY()
        FString OutputPath;

        // Folder for a numbered PNG per frame as well, for editing elsewhere.
        PROPERTY()
        FString PngDirectory;

        // Write a PNG for only one frame in this many, such as 15 for a review contact sheet of a long render.
        PROPERTY()
        int32 PngEvery = 1;

        PROPERTY()
        int32 Width = 1920;

        PROPERTY()
        int32 Height = 1080;

        PROPERTY()
        int32 FrameRate = 30;

        // Bits per second for the H.264 stream.
        PROPERTY()
        int32 BitRate = 24000000;

        // Length without a sequence, where zero records until movie.stop.
        PROPERTY()
        float Seconds = 0.0f;

        // Frames thrown away first, so streaming and exposure settle before the first kept frame.
        PROPERTY()
        int32 WarmupFrames = 30;

        // Record the game's sound into the MP4 as AAC, in step with the frames.
        PROPERTY()
        bool bAudio = true;

        // Also write the recorded mix to this WAV.
        PROPERTY()
        FString WavPath;
    };

    REFLECT()
    struct MCPEDITOR_API SMovieStatusResult
    {
        GENERATED_BODY()

        // Whether the file carries a sound track.
        PROPERTY()
        bool bAudio = false;

        PROPERTY()
        bool bActive = false;

        PROPERTY()
        bool bFinished = false;

        PROPERTY()
        int32 FramesWritten = 0;

        PROPERTY()
        int32 FrameCount = 0;

        PROPERTY()
        FString OutputPath;

        PROPERTY()
        FString PngDirectory;

        // Size of the picture being read back before it is cropped and scaled.
        PROPERTY()
        int32 SourceWidth = 0;

        PROPERTY()
        int32 SourceHeight = 0;

        PROPERTY()
        FString Error;
    };

    REFLECT()
    struct MCPEDITOR_API SCameraViewParams
    {
        GENERATED_BODY()

        // Look at the editor's world even while a game is playing.
        PROPERTY()
        bool bEditorWorld = false;
    };

    REFLECT()
    struct MCPEDITOR_API SCameraViewResult
    {
        GENERATED_BODY()

        // Game or Editor.
        PROPERTY()
        FString World;

        PROPERTY()
        TVector<float> Location;

        // Pitch, yaw and roll in degrees, the order a Transform track's Rotation takes.
        PROPERTY()
        TVector<float> Rotation;

        PROPERTY()
        TVector<float> Forward;

        PROPERTY()
        float FOV = 0.0f;

        // A ready shot key for this view, to paste into sequence.shot.
        PROPERTY()
        FString ShotKey;
    };

    REFLECT()
    struct MCPEDITOR_API SGroundHeightParams
    {
        GENERATED_BODY()

        // Array of [x,z] points to probe.
        PROPERTY(RawJson)
        FString Points;
    };

    REFLECT()
    struct MCPEDITOR_API SGroundHeightResult
    {
        GENERATED_BODY()

        // Height of the first surface under each point, in the same order, or a large negative number over nothing.
        PROPERTY()
        TVector<float> Heights;
    };

    namespace MCP
    {
        void RegisterSequenceTools(FStringView Owner);
    }
}
