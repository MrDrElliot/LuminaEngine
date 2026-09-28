#pragma once
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

// Records what the playing world shows into a video, one frame per engine frame at a locked time step.
namespace Lumina::MovieCapture
{
    struct FSettings
    {
        // MP4 to write; empty writes only the PNG sequence.
        FString OutputPath;

        // Folder for a numbered PNG per frame, for editing elsewhere; empty writes none.
        FString PngDirectory;

        // Writes a PNG for one frame in this many, so a long render can be reviewed without a full image sequence.
        uint32  PngEvery = 1;

        uint32  Width = 1920;
        uint32  Height = 1080;
        uint32  FrameRate = 30;
        uint32  BitRate = 24000000;

        // Frames to record before stopping on its own, where zero records until Stop.
        uint32  FrameCount = 0;

        // Frames rendered and thrown away first, so the picture has settled before the first one is kept.
        uint32  WarmupFrames = 3;

        // Records the game's mix into the MP4, advanced one video frame at a time so it stays in sync however slowly frames draw.
        bool    bRecordAudio = true;

        // Also writes the recorded mix as a 16-bit WAV, for editing the sound separately from the picture.
        FString WavPath;

        // Run once, on the first frame that is kept, which is where a sequence should start.
        TFunction<void()> OnRecordingStarted;

        // Run once the file is finished.
        TFunction<void()> OnRecordingFinished;
    };

    struct FStatus
    {
        bool    bActive = false;
        bool    bFinished = false;
        uint32  FramesWritten = 0;
        uint32  FrameCount = 0;
        uint32  SourceWidth = 0;
        uint32  SourceHeight = 0;
        bool    bAudio = false;
        FString OutputPath;
        FString PngDirectory;
        FString Error;
    };

    EDITOR_API bool Start(FSettings Settings, FString& OutError);

    // Finishes the file with whatever has been recorded.
    EDITOR_API void Stop();

    EDITOR_API FStatus GetStatus();

    // Called once per editor frame; records the frame the world last rendered.
    EDITOR_API void Tick();
}
