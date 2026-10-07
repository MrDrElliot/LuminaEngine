#pragma once

#include "Audio/AudioDecode.h"
#include "Audio/AudioTypes.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "imgui.h"

namespace Lumina
{
    // Editor for CAudioStream: waveform visualization plus an in-editor playback preview.
    // PCM is decoded once on init into fixed-resolution min/max peak buckets per channel.
    class FAudioStreamEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FAudioStreamEditorTool)

        FAudioStreamEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_WAVEFORM; }
        // Bulk data, so a whole-asset snapshot per edit would cost more than the undo is worth.
        bool WantsAutomaticAssetUndo() override { return false; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        void BuildPeaks();
        void DrawWaveform();
        void DrawDetails();

        void StartPreview();
        void StopPreview();
        // Sets the playhead; seeks the live preview when playing.
        void SetScrubTime(float Time);
        // Current preview time in seconds, advancing bPreviewPlaying; handles loop wrap and end-stop.
        float TickPlayhead();

        static constexpr uint32 NumPeakBuckets = 4096;

        Audio::FAudioInfo Info;
        // Per-channel min/max pairs: [Channel * NumPeakBuckets + Bucket].
        TVector<float> PeakMin;
        TVector<float> PeakMax;
        bool bPeaksValid = false;

        FAudioHandle PreviewHandle;
        bool   bPreviewPlaying = false;
        bool   bPreviewLoop    = false;
        float  PreviewVolume   = 1.0f;
        // Wall-clock anchor: playhead = now - PreviewStartTime (scrubbing rebases it).
        double PreviewStartTime = 0.0;
        // Playhead position while stopped; playback starts from here.
        float  ScrubTime = 0.0f;
    };
}
