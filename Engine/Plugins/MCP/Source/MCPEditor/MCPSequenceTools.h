#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPSequenceTools.generated.h"

namespace Lumina
{
    REFLECT()
    enum class ESequenceBindingAction : uint8
    {
        Add,
        Remove,
    };

    REFLECT()
    enum class ESequenceTrackAction : uint8
    {
        Add,
        Remove,
    };

    REFLECT()
    enum class ESequencePlaybackAction : uint8
    {
        Play,
        Pause,
        Resume,
        Stop,
        Seek,
        Restart,
        State,
    };

    REFLECT()
    enum class EMovieRenderAction : uint8
    {
        Start,
        Status,
        Stop,
    };

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

        // Seconds. A shot past the end lengthens it.
        PROPERTY()
        float Duration = 10.0f;

        PROPERTY()
        int32 FrameRate = 30;

        // Aspect between black bars, such as 2.39. Zero shows none.
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

        // Every field of the track as JSON, keys included unless bSummary was set.
        PROPERTY()
        FString Values;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeSequenceParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Sequence;

        // Report each curve as its key count and range rather than every key.
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

        // Name to pass to sequence.track Add.
        PROPERTY()
        FString Name;

        PROPERTY()
        FString Description;

        // Needs a binding to drive, rather than acting on the whole sequence.
        PROPERTY()
        bool bNeedsBinding = false;

        // JSON Schema of the fields sequence.set_track_property and sequence.track Settings accept.
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
    struct MCPEDITOR_API SSequenceBindingParams
    {
        GENERATED_BODY()

        PROPERTY()
        ESequenceBindingAction Action = ESequenceBindingAction::Add;

        PROPERTY()
        FString Sequence;

        // Add only. The binding's name; for Possess, the name of the world entity to drive.
        PROPERTY()
        FString Name;

        // Add only. Possess an entity, Spawn a prefab for the sequence's length, or Camera for a cinematic camera.
        PROPERTY()
        FString Kind = "Possess";

        // Add only. GUID of the prefab a Spawn binding creates.
        PROPERTY()
        FString Prefab;

        // Remove only. Name or index; its tracks and cuts go with it.
        PROPERTY()
        FString Binding;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceIndexResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Index = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SSequenceTrackParams
    {
        GENERATED_BODY()

        PROPERTY()
        ESequenceTrackAction Action = ESequenceTrackAction::Add;

        PROPERTY()
        FString Sequence;

        // Add only. From sequence.track_types, such as Transform, Property, Fade or LookAt.
        PROPERTY()
        FString Type;

        // Add only. Name or index of the binding to drive; empty for a sequence-wide track.
        PROPERTY()
        FString Binding;

        // Add only. Initial fields as a JSON object of property paths, such as {"PropertyPath":"FOV"}.
        PROPERTY(RawJson)
        FString Settings;

        // Remove only. Track index; negative removes every track.
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

        // Array of {Time, Value, Interp?}; Value is a number, or [x,y,z] for a vector channel.
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

        // What gameplay listens for through World.GetSequences().ConsumeEvents.
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

        // Camera binding, created when missing. Empty names a new one after the shot count.
        PROPERTY()
        FString Camera;

        PROPERTY()
        float Start = 0.0f;

        PROPERTY()
        float End = 3.0f;

        // Camera keys: Time (from Start) and Location, plus any of Rotation, Target, FOV, Focus, FStop, Ground.
        PROPERTY(RawJson)
        FString Keys;

        // Binding the camera follows with its aim for the whole shot, overriding key rotations.
        PROPERTY()
        FString LookAt;

        // Fixed [x,y,z] point to aim at when there is no LookAt binding.
        PROPERTY(RawJson)
        FString LookAtPoint;

        // [x,y,z] added to the LookAt binding's position, to frame a chest rather than the feet.
        PROPERTY(RawJson)
        FString LookAtOffset;

        // Vertical FOV in degrees for the shot unless keys say otherwise. Zero keeps the camera's.
        PROPERTY()
        float FOV = 0.0f;

        // Aperture for depth of field, such as 2.8. Zero leaves it off.
        PROPERTY()
        float FStop = 0.0f;

        // With a look-at and an aperture, keep focus on the subject.
        PROPERTY()
        bool bAutoFocus = true;

        // Handheld drift: 1 is a steady operator, 3 running, 0 locked off.
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

        // Read every height in the shot as above the terrain. A key's own Ground overrides it.
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
    struct MCPEDITOR_API SSequencePlaybackParams
    {
        GENERATED_BODY()

        PROPERTY()
        ESequencePlaybackAction Action = ESequencePlaybackAction::Play;

        // The sequence to Play. For the control actions, which players; empty means all.
        PROPERTY()
        FString Sequence;

        // Play only.
        PROPERTY()
        bool bLoop = false;

        // Play only.
        PROPERTY()
        float PlayRate = 1.0f;

        // Play only.
        PROPERTY()
        float StartTime = 0.0f;

        // Play only. Bind and show StartTime without advancing, for framing a shot.
        PROPERTY()
        bool bPaused = false;

        // Seek only. Seconds.
        PROPERTY()
        float Time = 0.0f;
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

        PROPERTY()
        EMovieRenderAction Action = EMovieRenderAction::Start;

        // Start only. Sequence to play start to end. Empty records whatever the game shows for Seconds.
        PROPERTY()
        FString Sequence;

        // MP4 to write. Empty with no PngDirectory writes to the engine's Saved/Movies.
        PROPERTY()
        FString OutputPath;

        // Folder for a numbered PNG per frame as well.
        PROPERTY()
        FString PngDirectory;

        // Write a PNG for one frame in this many, such as 15 for a contact sheet.
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

        // Length without a sequence; zero records until Stop.
        PROPERTY()
        float Seconds = 0.0f;

        // Frames discarded first so streaming and exposure settle.
        PROPERTY()
        int32 WarmupFrames = 30;

        // Record the game's sound into the MP4 as AAC.
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

        PROPERTY()
        int32 SourceWidth = 0;

        PROPERTY()
        int32 SourceHeight = 0;

        PROPERTY()
        FString Error;
    };

    namespace MCP
    {
        void RegisterSequenceTools(FStringView Owner);
    }
}
