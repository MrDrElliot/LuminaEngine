#include "AudioStreamEditorTool.h"

#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Audio/AudioGlobals.h"
#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    static const char* WaveformWindowName = "Waveform";
    static const char* DetailsWindowName  = "Details";

    FAudioStreamEditorTool::FAudioStreamEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FAudioStreamEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        BuildPeaks();

        CreateToolWindow(WaveformWindowName, [this](bool /*bFocused*/)
        {
            DrawWaveform();
        });

        CreateToolWindow(DetailsWindowName, [this](bool /*bFocused*/)
        {
            DrawDetails();
        });
    }

    void FAudioStreamEditorTool::OnDeinitialize(const FUpdateContext& /*UpdateContext*/)
    {
        StopPreview();
    }

    void FAudioStreamEditorTool::BuildPeaks()
    {
        bPeaksValid = false;

        CAudioStream* Stream = GetAsset<CAudioStream>();
        if (Stream == nullptr || !Stream->IsValid())
        {
            return;
        }

        Audio::FAudioInfo DecodedInfo;
        TVector<float> Samples;
        if (!Audio::DecodePCM(Stream->AudioData->Bytes.data(), Stream->AudioData->Bytes.size(), DecodedInfo, Samples))
        {
            return;
        }

        Info = DecodedInfo;

        const uint32 Channels = Info.NumChannels;
        const uint64 Frames   = Info.NumFrames;

        PeakMin.assign((size_t)Channels * NumPeakBuckets, 0.0f);
        PeakMax.assign((size_t)Channels * NumPeakBuckets, 0.0f);

        for (uint32 Bucket = 0; Bucket < NumPeakBuckets; ++Bucket)
        {
            const uint64 Begin = Frames * Bucket / NumPeakBuckets;
            const uint64 End   = Math::Max(Frames * (Bucket + 1) / NumPeakBuckets, Begin + 1);

            for (uint32 C = 0; C < Channels; ++C)
            {
                float MinS = 0.0f, MaxS = 0.0f;
                for (uint64 F = Begin; F < End && F < Frames; ++F)
                {
                    const float S = Samples[(size_t)F * Channels + C];
                    MinS = Math::Min(MinS, S);
                    MaxS = Math::Max(MaxS, S);
                }
                PeakMin[(size_t)C * NumPeakBuckets + Bucket] = MinS;
                PeakMax[(size_t)C * NumPeakBuckets + Bucket] = MaxS;
            }
        }

        bPeaksValid = true;
    }

    void FAudioStreamEditorTool::StartPreview()
    {
        CAudioStream* Stream = GetAsset<CAudioStream>();
        if (!Audio::HasDevice() || Stream == nullptr || !Stream->IsValid())
        {
            return;
        }

        StopPreview();

        const float Duration = (float)Info.GetDuration();
        if (ScrubTime >= Duration)
        {
            ScrubTime = 0.0f;
        }

        const uint64 StartFrame = (uint64)((double)ScrubTime * Info.SampleRate);
        PreviewHandle = Audio::Context().PlayAudio2D(Stream->GetAudioData(), PreviewVolume, 1.0f, bPreviewLoop, StartFrame);
        bPreviewPlaying  = PreviewHandle.IsValid();
        PreviewStartTime = ImGui::GetTime() - (double)ScrubTime;
    }

    void FAudioStreamEditorTool::StopPreview()
    {
        if (Audio::HasDevice() && PreviewHandle.IsValid())
        {
            Audio::Context().StopSound(PreviewHandle);
        }
        PreviewHandle   = FAudioHandle::Invalid();
        bPreviewPlaying = false;
    }

    void FAudioStreamEditorTool::SetScrubTime(float Time)
    {
        const float Duration = (float)Info.GetDuration();
        ScrubTime = Math::Clamp(Time, 0.0f, Duration);

        if (bPreviewPlaying && Audio::HasDevice() && PreviewHandle.IsValid())
        {
            Audio::Context().SeekToFrame(PreviewHandle, (uint64)((double)ScrubTime * Info.SampleRate));
            PreviewStartTime = ImGui::GetTime() - (double)ScrubTime;
        }
    }

    float FAudioStreamEditorTool::TickPlayhead()
    {
        if (!bPreviewPlaying)
        {
            return ScrubTime;
        }

        const float Duration = (float)Info.GetDuration();
        if (Duration <= 0.0f)
        {
            return 0.0f;
        }

        if (Audio::Context().GetVoiceState(PreviewHandle) != EAudioVoiceState::Free)
        {
            // Real cursor from the mixer, so the playhead can't drift from what's audible.
            const float Elapsed = (float)((double)Audio::Context().GetPlaybackFrame(PreviewHandle) / (double)Info.SampleRate);
            return bPreviewLoop ? fmodf(Elapsed, Duration) : Math::Min(Elapsed, Duration);
        }

        // Falls back to the wall clock for the frame or two before the pump starts the voice.
        float Elapsed = (float)(ImGui::GetTime() - PreviewStartTime);
        if (bPreviewLoop)
        {
            return fmodf(Elapsed, Duration);
        }

        if (Elapsed >= Duration)
        {
            PreviewHandle   = FAudioHandle::Invalid();
            bPreviewPlaying = false;
            return ScrubTime;
        }
        return Elapsed;
    }

    void FAudioStreamEditorTool::DrawWaveform()
    {
        CAudioStream* Stream = GetAsset<CAudioStream>();
        if (Stream == nullptr || !bPeaksValid)
        {
            ImGui::TextDisabled("No decodable audio data.");
            return;
        }

        const float PlayheadTime = TickPlayhead();

        // Transport bar.
        {
            if (bPreviewPlaying)
            {
                if (ImGui::Button(LE_ICON_STOP " Stop"))
                {
                    StopPreview();
                }
            }
            else
            {
                if (ImGui::Button(LE_ICON_PLAY " Play"))
                {
                    StartPreview();
                }
            }

            ImGui::SameLine(0, 12);
            if (ImGui::Checkbox("Loop", &bPreviewLoop) && bPreviewPlaying)
            {
                Audio::Context().SetLooping(PreviewHandle, bPreviewLoop);
            }

            ImGui::SameLine(0, 12);
            ImGui::SetNextItemWidth(140);
            if (ImGui::SliderFloat("Volume", &PreviewVolume, 0.0f, 1.0f, "%.2f") && bPreviewPlaying)
            {
                Audio::Context().SetVolume(PreviewHandle, PreviewVolume);
            }

            ImGui::SameLine(0, 20);
            const float Duration = (float)Info.GetDuration();
            ImGui::Text("%6.2fs / %.2fs", PlayheadTime, Duration);
        }

        ImGui::Spacing();

        const ImVec2 Avail = ImGui::GetContentRegionAvail();
        if (Avail.x < 16.0f || Avail.y < 16.0f)
        {
            return;
        }

        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        ImDrawList* DrawList = ImGui::GetWindowDrawList();

        // Click or drag anywhere on the waveform to move the playhead.
        ImGui::InvisibleButton("##WaveformScrub", Avail);
        if (ImGui::IsItemActive())
        {
            const float Duration = (float)Info.GetDuration();
            const float T = Math::Clamp((ImGui::GetMousePos().x - Origin.x) / Avail.x, 0.0f, 1.0f);
            SetScrubTime(T * Duration);
        }

        const ImU32 BgColor       = IM_COL32(24, 24, 28, 255);
        const ImU32 CenterColor   = IM_COL32(80, 80, 90, 255);
        const ImU32 WaveColor     = IM_COL32(96, 200, 255, 255);
        const ImU32 PlayheadColor = IM_COL32(255, 180, 60, 255);
        const ImU32 BorderColor   = IM_COL32(100, 100, 120, 255);

        DrawList->AddRectFilled(Origin, ImVec2(Origin.x + Avail.x, Origin.y + Avail.y), BgColor);

        const uint32 Channels = Info.NumChannels;
        const float LaneH     = Avail.y / (float)Channels;

        for (uint32 C = 0; C < Channels; ++C)
        {
            const float LaneTop    = Origin.y + LaneH * (float)C;
            const float LaneCenter = LaneTop + LaneH * 0.5f;
            const float Amp        = LaneH * 0.5f - 2.0f;

            DrawList->AddLine(ImVec2(Origin.x, LaneCenter), ImVec2(Origin.x + Avail.x, LaneCenter), CenterColor);

            const float* Mins = &PeakMin[(size_t)C * NumPeakBuckets];
            const float* Maxs = &PeakMax[(size_t)C * NumPeakBuckets];

            const int32 NumColumns = (int32)Avail.x;
            for (int32 X = 0; X < NumColumns; ++X)
            {
                const uint32 BucketBegin = (uint32)((uint64)NumPeakBuckets * X / NumColumns);
                const uint32 BucketEnd   = Math::Max((uint32)((uint64)NumPeakBuckets * (X + 1) / NumColumns), BucketBegin + 1);

                float MinS = 0.0f, MaxS = 0.0f;
                for (uint32 B = BucketBegin; B < BucketEnd && B < NumPeakBuckets; ++B)
                {
                    MinS = Math::Min(MinS, Mins[B]);
                    MaxS = Math::Max(MaxS, Maxs[B]);
                }

                const float PixelX = Origin.x + (float)X;
                DrawList->AddLine(
                    ImVec2(PixelX, LaneCenter - MaxS * Amp),
                    ImVec2(PixelX, LaneCenter - MinS * Amp),
                    WaveColor);
            }

            if (C > 0)
            {
                DrawList->AddLine(ImVec2(Origin.x, LaneTop), ImVec2(Origin.x + Avail.x, LaneTop), BorderColor);
            }
        }

        {
            const float Duration = (float)Info.GetDuration();
            if (Duration > 0.0f)
            {
                // Live time while playing; parked at the scrub position otherwise.
                const float ShownTime = bPreviewPlaying ? PlayheadTime : ScrubTime;
                const float PlayheadX = Origin.x + Avail.x * (ShownTime / Duration);
                DrawList->AddLine(ImVec2(PlayheadX, Origin.y), ImVec2(PlayheadX, Origin.y + Avail.y), PlayheadColor, 2.0f);
            }
        }

        DrawList->AddRect(Origin, ImVec2(Origin.x + Avail.x, Origin.y + Avail.y), BorderColor);
    }

    void FAudioStreamEditorTool::DrawDetails()
    {
        CAudioStream* Stream = GetAsset<CAudioStream>();
        if (Stream == nullptr)
        {
            return;
        }

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", Stream->GetName().c_str());
        ImGui::SeparatorText("Audio Information");
        ImGuiX::Font::PopFont();

        ImGui::Spacing();

        if (ImGui::BeginTable("##AudioInfo", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            auto PropertyRow = [](const char* Label, const FString& Value)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(Label);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(Value.c_str());
            };

            const float Duration = Stream->GetDuration();
            char DurationStr[64];
            if (Duration >= 60.0f)
            {
                snprintf(DurationStr, sizeof(DurationStr), "%d:%05.2f", (int)(Duration / 60.0f), fmodf(Duration, 60.0f));
            }
            else
            {
                snprintf(DurationStr, sizeof(DurationStr), "%.2f s", Duration);
            }

            PropertyRow("Duration", DurationStr);
            PropertyRow("Sample Rate", Format("{}", Stream->SampleRate) + " Hz");
            PropertyRow("Channels", Stream->NumChannels == 1 ? "1 (Mono)"
                                  : Stream->NumChannels == 2 ? "2 (Stereo)"
                                  : Format("{}", Stream->NumChannels));
            PropertyRow("Frames", Format("{}", Stream->NumFrames));

            const size_t SizeBytes = Stream->IsValid() ? Stream->AudioData->Bytes.size() : 0;
            char SizeStr[64];
            if (SizeBytes >= 1024 * 1024)
            {
                snprintf(SizeStr, sizeof(SizeStr), "%.2f MB", (double)SizeBytes / (1024.0 * 1024.0));
            }
            else
            {
                snprintf(SizeStr, sizeof(SizeStr), "%.1f KB", (double)SizeBytes / 1024.0);
            }
            PropertyRow("Source Size", SizeStr);

            ImGui::EndTable();
        }

        ImGui::Spacing();

        if (!Stream->SourcePath.empty())
        {
            ImGui::TextDisabled("Source: %s", Stream->SourcePath.c_str());
        }
    }

    void FAudioStreamEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Preview",
            "Play / Stop previews the clip 2D (no spatialization) through the editor's audio device. "
            "Loop and Volume apply live to the playing preview.");
        DrawHelpTextRow("Waveform",
            "Each channel gets its own lane showing per-column min/max sample peaks. "
            "The orange line is the playhead while previewing.");
        DrawHelpTextRow("Scrubbing",
            "Click or drag anywhere on the waveform to move the playhead. While playing, the preview "
            "seeks live; while stopped, playback starts from the scrub position.");
    }

    void FAudioStreamEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& /*InDockspaceSize*/) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftDockID = 0, RightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.3f, &RightDockID, &LeftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(WaveformWindowName).c_str(), LeftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(DetailsWindowName).c_str(), RightDockID);
    }
}
