#include "EditorPCH.h"
#include "MovieCapture.h"

#include "Audio/AudioGlobals.h"
#include "Containers/StringFormat.h"
#include "Core/Console/ConsoleVariable.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "ScreenshotCapture.h"
#include "Tools/Image/ImageWrite.h"
#include "VideoEncoder.h"

namespace Lumina::MovieCapture
{
    namespace
    {
        constexpr const char* FixedDeltaVariable = "Core.FixedDeltaTime";
        constexpr const char* MaxFpsVariable = "Core.MaxFPS";

        struct FState
        {
            FSettings     Settings;
            FStatus       Status;
            FVideoEncoder Encoder;
            uint32        WarmupRemaining = 0;
            bool          bStartedCallback = false;
            FString       PreviousFixedDelta;
            FString       PreviousMaxFps;
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
            if (!Value.empty())
            {
                FConsoleRegistry::Get().SetValueFromString(Name, Value);
            }
        }

        // The largest centered region of Source with the output's aspect, scaled to the output with bilinear filtering.
        void CropAndScale(const TVector<uint8>& Source, uint32 SourceWidth, uint32 SourceHeight, TVector<uint8>& Out, uint32 Width, uint32 Height)
        {
            const float TargetAspect = (float)Width / (float)Height;
            float RegionWidth = (float)SourceWidth;
            float RegionHeight = (float)SourceHeight;
            if (RegionWidth / RegionHeight > TargetAspect)
            {
                RegionWidth = RegionHeight * TargetAspect;
            }
            else
            {
                RegionHeight = RegionWidth / TargetAspect;
            }
            const float OffsetX = ((float)SourceWidth - RegionWidth) * 0.5f;
            const float OffsetY = ((float)SourceHeight - RegionHeight) * 0.5f;
            const float StepX = RegionWidth / (float)Width;
            const float StepY = RegionHeight / (float)Height;

            Out.resize((size_t)Width * Height * 4u);
            for (uint32 Y = 0; Y < Height; ++Y)
            {
                const float SY = Math::Clamp(OffsetY + ((float)Y + 0.5f) * StepY - 0.5f, 0.0f, (float)SourceHeight - 1.0f);
                const uint32 Y0 = (uint32)SY;
                const uint32 Y1 = Math::Min(Y0 + 1u, SourceHeight - 1u);
                const float FY = SY - (float)Y0;
                for (uint32 X = 0; X < Width; ++X)
                {
                    const float SX = Math::Clamp(OffsetX + ((float)X + 0.5f) * StepX - 0.5f, 0.0f, (float)SourceWidth - 1.0f);
                    const uint32 X0 = (uint32)SX;
                    const uint32 X1 = Math::Min(X0 + 1u, SourceWidth - 1u);
                    const float FX = SX - (float)X0;
                    for (uint32 Channel = 0; Channel < 4; ++Channel)
                    {
                        const float A = Source[((size_t)Y0 * SourceWidth + X0) * 4 + Channel];
                        const float B = Source[((size_t)Y0 * SourceWidth + X1) * 4 + Channel];
                        const float C = Source[((size_t)Y1 * SourceWidth + X0) * 4 + Channel];
                        const float D = Source[((size_t)Y1 * SourceWidth + X1) * 4 + Channel];
                        const float Top = A + (B - A) * FX;
                        const float Bottom = C + (D - C) * FX;
                        Out[((size_t)Y * Width + X) * 4 + Channel] = (uint8)Math::Clamp(Top + (Bottom - Top) * FY + 0.5f, 0.0f, 255.0f);
                    }
                }
            }
        }

        void AppendLittleEndian(TVector<uint8>& Out, uint32 Value, uint32 Bytes)
        {
            for (uint32 Index = 0; Index < Bytes; ++Index)
            {
                Out.push_back((uint8)((Value >> (Index * 8u)) & 0xFFu));
            }
        }

        bool WriteWav(const FString& Path, const TVector<int16>& Samples, uint32 SampleRate, uint32 Channels)
        {
            const uint32 DataBytes = (uint32)(Samples.size() * sizeof(int16));
            TVector<uint8> File;
            File.reserve(44u + DataBytes);
            File.insert(File.end(), { 'R', 'I', 'F', 'F' });
            AppendLittleEndian(File, 36u + DataBytes, 4);
            File.insert(File.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
            AppendLittleEndian(File, 16u, 4);
            AppendLittleEndian(File, 1u, 2);
            AppendLittleEndian(File, Channels, 2);
            AppendLittleEndian(File, SampleRate, 4);
            AppendLittleEndian(File, SampleRate * Channels * 2u, 4);
            AppendLittleEndian(File, Channels * 2u, 2);
            AppendLittleEndian(File, 16u, 2);
            File.insert(File.end(), { 'd', 'a', 't', 'a' });
            AppendLittleEndian(File, DataBytes, 4);
            const uint8* Bytes = reinterpret_cast<const uint8*>(Samples.data());
            File.insert(File.end(), Bytes, Bytes + DataBytes);
            return FileHelper::SaveArrayToFile(File, FStringView(Path.c_str(), Path.size()));
        }

        void Finish(const FString& Error)
        {
            FState& S = State();
            if (!S.Status.bActive)
            {
                return;
            }

            FString CloseError;
            if (S.Encoder.IsOpen() && !S.Encoder.Close(CloseError) && Error.empty())
            {
                S.Status.Error = CloseError;
            }
            if (!Error.empty())
            {
                S.Status.Error = Error;
            }

            if (S.bOfflineAudio)
            {
                Audio::Context().EndOfflineRender();
                S.bOfflineAudio = false;
            }
            if (!S.Settings.WavPath.empty() && !S.WavSamples.empty())
            {
                if (!WriteWav(S.Settings.WavPath, S.WavSamples, S.AudioRate, S.AudioChannels))
                {
                    LOG_WARN("Movie capture could not write the sound to {}.", S.Settings.WavPath);
                }
            }
            S.WavSamples.clear();
            S.WavSamples.shrink_to_fit();

            WriteVariable(FixedDeltaVariable, S.PreviousFixedDelta.empty() ? FString("0") : S.PreviousFixedDelta);
            WriteVariable(MaxFpsVariable, S.PreviousMaxFps);

            S.Status.bActive = false;
            S.Status.bFinished = true;
            LOG_INFO("Movie capture finished with {} frames{}{}", S.Status.FramesWritten,
                S.Status.OutputPath.empty() ? FString() : FString(" to ") + S.Status.OutputPath,
                S.Status.Error.empty() ? FString() : FString(", ") + S.Status.Error);

            if (S.Settings.OnRecordingFinished)
            {
                TFunction<void()> Callback = Move(S.Settings.OnRecordingFinished);
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
        if (Settings.OutputPath.empty() && Settings.PngDirectory.empty())
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
        S.bOfflineAudio = Settings.bRecordAudio && (!Settings.OutputPath.empty() || !Settings.WavPath.empty()) && Audio::HasDevice()
                       && Audio::Context().BeginOfflineRender(S.AudioRate, S.AudioChannels);
        S.AudioTick = 0;

        if (!Settings.OutputPath.empty())
        {
            const FString Parent = Paths::Parent(FStringView(Settings.OutputPath.c_str(), Settings.OutputPath.size()), true);
            if (!Parent.empty())
            {
                Paths::CreateDirectories(FStringView(Parent.c_str(), Parent.size()));
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
        if (!Settings.PngDirectory.empty())
        {
            Paths::CreateDirectories(FStringView(Settings.PngDirectory.c_str(), Settings.PngDirectory.size()));
        }

        S.Status = FStatus{};
        S.Status.bActive = true;
        S.Status.FrameCount = Settings.FrameCount;
        S.Status.OutputPath = Settings.OutputPath;
        S.Status.PngDirectory = Settings.PngDirectory;
        S.Status.bAudio = S.bOfflineAudio;
        S.WarmupRemaining = Settings.WarmupFrames;
        S.bStartedCallback = false;

        // Every frame advances the game by exactly one video frame, however long it takes to draw and encode.
        S.PreviousFixedDelta = ReadVariable(FixedDeltaVariable);
        S.PreviousMaxFps = ReadVariable(MaxFpsVariable);
        FConsoleRegistry::Get().SetValueFromString(FixedDeltaVariable, Lumina::Format("{}", 1.0 / (double)Settings.FrameRate));
        FConsoleRegistry::Get().SetValueFromString(MaxFpsVariable, "0");

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
            S.AudioSlice.resize((size_t)AudioFrames * S.AudioChannels);
            Audio::Context().RenderOffline(S.AudioSlice.data(), AudioFrames);
        }

        if (S.WarmupRemaining > 0)
        {
            --S.WarmupRemaining;
            return;
        }

        if (!S.bStartedCallback)
        {
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
            Finish(Error.empty() ? FString("No world is rendering.") : Error);
            return;
        }
        S.Status.SourceWidth = SourceWidth;
        S.Status.SourceHeight = SourceHeight;

        CropAndScale(S.Source, SourceWidth, SourceHeight, S.Frame, S.Settings.Width, S.Settings.Height);

        if (S.Encoder.IsOpen() && !S.Encoder.WriteFrame(S.Frame.data(), Error))
        {
            Finish(Error);
            return;
        }
        if (S.Encoder.HasAudio() && !S.Encoder.WriteAudio(S.AudioSlice.data(), AudioFrames, Error))
        {
            Finish(Error);
            return;
        }
        if (S.bOfflineAudio && !S.Settings.WavPath.empty())
        {
            for (const float Sample : S.AudioSlice)
            {
                S.WavSamples.push_back((int16)(Math::Clamp(Sample, -1.0f, 1.0f) * 32767.0f));
            }
        }

        if (!S.Settings.PngDirectory.empty() && S.Status.FramesWritten % Math::Max(S.Settings.PngEvery, 1u) == 0)
        {
            const FString Path = Lumina::Format("{}/Frame_{:05}.png", S.Settings.PngDirectory, S.Status.FramesWritten);
            ImageWrite::WritePngFile(Path.c_str(), S.Settings.Width, S.Settings.Height, 4, S.Frame.data(), S.Settings.Width * 4);
        }

        ++S.Status.FramesWritten;
        if (S.Settings.FrameCount > 0 && S.Status.FramesWritten >= S.Settings.FrameCount)
        {
            Finish({});
        }
    }
}
