#include "EditorPCH.h"
#include "MovieCapture.h"

#include "Audio/AudioGlobals.h"
#include "Containers/StringFormat.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Engine/Engine.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "ScreenshotCapture.h"
#include "Tools/Image/ImageWrite.h"
#include "VideoEncoder.h"
#include "World/Scene/RenderScene/RenderScene.h"

namespace Lumina::MovieCapture
{
    namespace
    {
        constexpr const char* FixedDeltaVariable = "Core.FixedDeltaTime";

        // The renderer only resizes to an exact view once the size has held for 8 frames, so the letterboxed view needs a few more.
        constexpr uint32 LetterboxSettleFrames = 12;
        constexpr uint32 MaximumSizeWaitFrames = 120;

        struct FState
        {
            FSettings     Settings;
            FStatus       Status;
            FVideoEncoder Encoder;
            uint32        WarmupRemaining = 0;
            uint32        SizeWaitFrames = 0;
            bool          bStartedCallback = false;
            FString       PreviousFixedDelta;
            int32         PreviousFrameRateCap = -1;
            TVector<uint8> Source;
            TVector<uint8> Frame;

            bool           bOfflineAudio = false;
            uint32         AudioRate = 0;
            uint32         AudioChannels = 0;
            uint64         AudioTick = 0;
            TVector<float> AudioSlice;
            TVector<int16> WavSamples;
        };

        FState& State()
        {
            static FState Instance;
            return Instance;
        }

        FString ReadVariable(const char* Name)
        {
            const TOptional<FString> Value = FConsoleRegistry::Get().GetValueAsString(Name);
            return Value.IsSet() ? *Value : FString();
        }

        void WriteVariable(const char* Name, const FString& Value)
        {
            if (!Value.IsEmpty())
            {
                FConsoleRegistry::Get().SetValueFromString(Name, Value);
            }
        }

        void AppendLittleEndian(TVector<uint8>& Out, uint32 Value, uint32 Bytes)
        {
            for (uint32 Index = 0; Index < Bytes; ++Index)
            {
                Out.Add((uint8)((Value >> (Index * 8u)) & 0xFFu));
            }
        }

        bool WriteWav(const FString& Path, const TVector<int16>& Samples, uint32 SampleRate, uint32 Channels)
        {
            const uint32 DataBytes = (uint32)(Samples.Num() * sizeof(int16));
            TVector<uint8> File;
            File.Reserve(44u + DataBytes);
            File.Append({ 'R', 'I', 'F', 'F' });
            AppendLittleEndian(File, 36u + DataBytes, 4);
            File.Append({ 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
            AppendLittleEndian(File, 16u, 4);
            AppendLittleEndian(File, 1u, 2);
            AppendLittleEndian(File, Channels, 2);
            AppendLittleEndian(File, SampleRate, 4);
            AppendLittleEndian(File, SampleRate * Channels * 2u, 4);
            AppendLittleEndian(File, Channels * 2u, 2);
            AppendLittleEndian(File, 16u, 2);
            File.Append({ 'd', 'a', 't', 'a' });
            AppendLittleEndian(File, DataBytes, 4);
            const uint8* Bytes = reinterpret_cast<const uint8*>(Samples.GetData());
            File.Append(Bytes, DataBytes);
            return FileHelper::SaveArrayToFile(File, FStringView(*Path, Path.Len()));
        }

        void Finish(const FString& Error)
        {
            FState& S = State();
            if (!S.Status.bActive)
            {
                return;
            }

            FString CloseError;
            if (S.Encoder.IsOpen() && !S.Encoder.Close(CloseError) && Error.IsEmpty())
            {
                S.Status.Error = CloseError;
            }
            if (!Error.IsEmpty())
            {
                S.Status.Error = Error;
            }

            if (S.bOfflineAudio)
            {
                Audio::Context().EndOfflineRender();
                S.bOfflineAudio = false;
            }
            if (!S.Settings.WavPath.IsEmpty() && !S.WavSamples.IsEmpty())
            {
                if (!WriteWav(S.Settings.WavPath, S.WavSamples, S.AudioRate, S.AudioChannels))
                {
                    LOG_WARN("Movie capture could not write the sound to {}.", S.Settings.WavPath);
                }
            }
            S.WavSamples.Reset();
            S.WavSamples.Shrink();

            WriteVariable(FixedDeltaVariable, S.PreviousFixedDelta.IsEmpty() ? FString("0") : S.PreviousFixedDelta);
            if (GEngine != nullptr)
            {
                GEngine->SetFrameRateCapOverride(S.PreviousFrameRateCap);
            }

            S.Status.bActive = false;
            S.Status.bFinished = true;
            LOG_INFO("Movie capture finished with {} frames{}{}", S.Status.FramesWritten,
                S.Status.OutputPath.IsEmpty() ? FString() : FString(" to ") + S.Status.OutputPath,
                S.Status.Error.IsEmpty() ? FString() : FString(", ") + S.Status.Error);

            TFunction<void()> Callback = Move(S.Settings.OnRecordingFinished);

            // Released now rather than at exit, since a callback's code can live in a plugin that unloads before statics are destroyed.
            S.Settings = FSettings{};
            if (Callback)
            {
                Callback();
            }
        }
    }

    bool Start(FSettings Settings, FString& OutError)
    {
        FState& S = State();
        if (S.Status.bActive)
        {
            OutError = "A movie is already being recorded.";
            return false;
        }
        if (Settings.OutputPath.IsEmpty() && Settings.PngDirectory.IsEmpty())
        {
            OutError = "Give an output path, a PNG folder, or both.";
            return false;
        }

        Settings.Width &= ~1u;
        Settings.Height &= ~1u;
        Settings.FrameRate = Math::Clamp(Settings.FrameRate, 1u, 240u);
        if (Settings.Width < 16 || Settings.Height < 16)
        {
            OutError = "The output is too small to encode.";
            return false;
        }

        // Taken from the device before the file opens, since the encoder needs the mix format up front.
        S.bOfflineAudio = Settings.bRecordAudio && (!Settings.OutputPath.IsEmpty() || !Settings.WavPath.IsEmpty()) && Audio::HasDevice()
                       && Audio::Context().BeginOfflineRender(S.AudioRate, S.AudioChannels);
        S.AudioTick = 0;

        if (!Settings.OutputPath.IsEmpty())
        {
            const FString Parent = Paths::Parent(FStringView(*Settings.OutputPath, Settings.OutputPath.Len()), true);
            if (!Parent.IsEmpty())
            {
                Paths::CreateDirectories(FStringView(*Parent, Parent.Len()));
            }
            const uint32 Rate = S.bOfflineAudio ? S.AudioRate : 0u;
            if (!S.Encoder.Open(Settings.OutputPath, Settings.Width, Settings.Height, Settings.FrameRate, Settings.BitRate, OutError, Rate, S.AudioChannels))
            {
                if (S.bOfflineAudio)
                {
                    Audio::Context().EndOfflineRender();
                    S.bOfflineAudio = false;
                }
                return false;
            }
        }
        if (!Settings.PngDirectory.IsEmpty())
        {
            Paths::CreateDirectories(FStringView(*Settings.PngDirectory, Settings.PngDirectory.Len()));
        }

        S.Status = FStatus{};
        S.Status.bActive = true;
        S.Status.FrameCount = Settings.FrameCount;
        S.Status.OutputPath = Settings.OutputPath;
        S.Status.PngDirectory = Settings.PngDirectory;
        S.Status.bAudio = S.bOfflineAudio;
        S.WarmupRemaining = Math::Max(Settings.WarmupFrames, LetterboxSettleFrames);
        S.SizeWaitFrames = 0;
        S.bStartedCallback = false;

        // Every frame advances the game by exactly one video frame, however long it takes to draw and encode.
        S.PreviousFixedDelta = ReadVariable(FixedDeltaVariable);
        FConsoleRegistry::Get().SetValueFromString(FixedDeltaVariable, Lumina::Format("{}", 1.0 / (double)Settings.FrameRate));
        if (GEngine != nullptr)
        {
            S.PreviousFrameRateCap = GEngine->GetFrameRateCapOverride();
            GEngine->SetFrameRateCapOverride(0);
        }

        S.Settings = Move(Settings);
        return true;
    }

    void Stop()
    {
        Finish({});
    }

    FStatus GetStatus()
    {
        return State().Status;
    }

    float GetRecordingAspect()
    {
        const FState& S = State();
        return S.Status.bActive ? (float)S.Settings.Width / (float)S.Settings.Height : 0.0f;
    }

    bool GetRecordingSize(IRenderScene* Scene, uint32& OutWidth, uint32& OutHeight)
    {
        const FState& S = State();
        if (!S.Status.bActive || Scene == nullptr || Scene != Screenshot::FindActiveRenderScene())
        {
            return false;
        }
        OutWidth = S.Settings.Width;
        OutHeight = S.Settings.Height;
        return true;
    }

    void Tick()
    {
        FState& S = State();
        if (!S.Status.bActive)
        {
            return;
        }

        // Every tick advances the mix by one video frame, kept or not, split so the running total never drifts from the frame count.
        uint32 AudioFrames = 0;
        if (S.bOfflineAudio)
        {
            const uint64 Rate = S.AudioRate;
            const uint64 Fps = S.Settings.FrameRate;
            AudioFrames = (uint32)(Rate * (S.AudioTick + 1) / Fps - Rate * S.AudioTick / Fps);
            ++S.AudioTick;
            S.AudioSlice.SetNum(IntCastChecked<int32>(static_cast<int64>(AudioFrames) * S.AudioChannels));
            Audio::Context().RenderOffline(S.AudioSlice.GetData(), AudioFrames);
        }

        if (S.WarmupRemaining > 0)
        {
            --S.WarmupRemaining;
            return;
        }

        if (!S.bStartedCallback)
        {
            IRenderScene* Scene = Screenshot::FindActiveRenderScene();
            if (Scene == nullptr)
            {
                Finish("No world is rendering.");
                return;
            }
            const FUIntVector2 Extent = Scene->GetRenderExtent();
            if (!Scene->HasCompositedFrame() || Extent.x != S.Settings.Width || Extent.y != S.Settings.Height)
            {
                if (++S.SizeWaitFrames >= MaximumSizeWaitFrames)
                {
                    Finish(Lumina::Format("The scene did not reach {}x{} for recording.", S.Settings.Width, S.Settings.Height));
                }
                return;
            }
            S.bStartedCallback = true;
            if (S.Settings.OnRecordingStarted)
            {
                S.Settings.OnRecordingStarted();
            }
            // The frame on screen now predates the start, so the first kept frame is the next one.
            return;
        }

        IRenderScene* Scene = Screenshot::FindActiveRenderScene();
        FString Error;
        uint32 SourceWidth = 0;
        uint32 SourceHeight = 0;
        if (Scene == nullptr || !Screenshot::ReadDisplayPixels(Scene, S.Source, SourceWidth, SourceHeight, Error))
        {
            Finish(Error.IsEmpty() ? FString("No world is rendering.") : Error);
            return;
        }
        S.Status.SourceWidth = SourceWidth;
        S.Status.SourceHeight = SourceHeight;

        if (SourceWidth != S.Settings.Width || SourceHeight != S.Settings.Height)
        {
            Finish(Lumina::Format("The scene rendered at {}x{} instead of {}x{}.",
                SourceWidth, SourceHeight, S.Settings.Width, S.Settings.Height));
            return;
        }
        swap(S.Frame, S.Source);

        if (S.Encoder.IsOpen() && !S.Encoder.WriteFrame(S.Frame.GetData(), Error))
        {
            Finish(Error);
            return;
        }
        if (S.Encoder.HasAudio() && !S.Encoder.WriteAudio(S.AudioSlice.GetData(), AudioFrames, Error))
        {
            Finish(Error);
            return;
        }
        if (S.bOfflineAudio && !S.Settings.WavPath.IsEmpty())
        {
            for (const float Sample : S.AudioSlice)
            {
                S.WavSamples.Add((int16)(Math::Clamp(Sample, -1.0f, 1.0f) * 32767.0f));
            }
        }

        if (!S.Settings.PngDirectory.IsEmpty() && S.Status.FramesWritten % Math::Max(S.Settings.PngEvery, 1u) == 0)
        {
            const FString Path = Lumina::Format("{}/Frame_{:05}.png", S.Settings.PngDirectory, S.Status.FramesWritten);
            ImageWrite::WritePngFile(*Path, S.Settings.Width, S.Settings.Height, 4, S.Frame.GetData(), S.Settings.Width * 4);
        }

        ++S.Status.FramesWritten;
        if (S.Settings.FrameCount > 0 && S.Status.FramesWritten >= S.Settings.FrameCount)
        {
            Finish({});
        }
    }
}
