#include "CurveGradientCustomization.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "CoreTypeCustomization.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        constexpr int32 kThumbnailSamples = 48;
        constexpr float kStopHandleHalf   = 6.0f;

        ImU32 ToColor(const FVector4& C)
        {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(C.x, C.y, C.z, C.w));
        }

        // Alpha must read as transparency, or a fade-to-zero looks identical to a fade-to-black.
        void DrawCheckerboard(ImDrawList* DrawList, const ImVec2& Min, const ImVec2& Max)
        {
            constexpr float Cell = 6.0f;
            const ImU32 Light = IM_COL32(120, 120, 120, 255);
            const ImU32 Dark  = IM_COL32(80, 80, 80, 255);

            DrawList->PushClipRect(Min, Max, true);
            int32 Row = 0;
            for (float Y = Min.y; Y < Max.y; Y += Cell, ++Row)
            {
                int32 Col = 0;
                for (float X = Min.x; X < Max.x; X += Cell, ++Col)
                {
                    const ImVec2 CellMax(Math::Min(X + Cell, Max.x), Math::Min(Y + Cell, Max.y));
                    DrawList->AddRectFilled(ImVec2(X, Y), CellMax, ((Row + Col) & 1) ? Dark : Light);
                }
            }
            DrawList->PopClipRect();
        }
    }

    //~ SCurve =====================================================================================

    void FCurvePropertyCustomization::DrawThumbnail(const SKeyedCurve& InCurve, const ImVec2& Size) const
    {
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const ImVec2 Min = ImGui::GetCursorScreenPos();
        const ImVec2 Max(Min.x + Size.x, Min.y + Size.y);

        DrawList->AddRectFilled(Min, Max, IM_COL32(28, 28, 32, 255), 3.0f);
        DrawList->AddRect(Min, Max, IM_COL32(70, 70, 78, 255), 3.0f);

        if (InCurve.IsEmpty())
        {
            const char* Empty = "empty";
            const ImVec2 TextSize = ImGui::CalcTextSize(Empty);
            DrawList->AddText(ImVec2(Min.x + (Size.x - TextSize.x) * 0.5f, Min.y + (Size.y - TextSize.y) * 0.5f),
                IM_COL32(140, 140, 148, 255), Empty);
            return;
        }

        float TimeMin = 0.0f, TimeMax = 1.0f;
        float ValueMin = 0.0f, ValueMax = 1.0f;
        InCurve.GetTimeRange(TimeMin, TimeMax);
        InCurve.GetValueRange(ValueMin, ValueMax);

        // A flat curve has no range to normalize against, so give it a band rather than divide by zero.
        if (TimeMax - TimeMin < 1e-6f)  { TimeMax  = TimeMin  + 1.0f; }
        if (ValueMax - ValueMin < 1e-6f) { ValueMin -= 0.5f; ValueMax += 0.5f; }

        constexpr float Pad = 3.0f;
        ImVec2 Points[kThumbnailSamples];
        for (int32 i = 0; i < kThumbnailSamples; ++i)
        {
            const float Alpha = (float)i / (float)(kThumbnailSamples - 1);
            const float Time  = TimeMin + (TimeMax - TimeMin) * Alpha;
            const float Norm  = (InCurve.Evaluate(Time) - ValueMin) / (ValueMax - ValueMin);

            Points[i].x = Min.x + Pad + (Size.x - Pad * 2.0f) * Alpha;
            // Screen Y grows downward, so a high value maps to a small Y.
            Points[i].y = Max.y - Pad - (Size.y - Pad * 2.0f) * Math::Clamp(Norm, 0.0f, 1.0f);
        }
        DrawList->AddPolyline(Points, kThumbnailSamples, IM_COL32(120, 190, 255, 255), 0, 1.5f);

        ImGui::Dummy(Size);
    }

    bool FCurvePropertyCustomization::DrawAssetSlot()
    {
        // The child handle is synthesized, which is what gives the picker its declared-class filter free.
        if (AssetPicker == nullptr)
        {
            FProperty* AssetProp = SCurve::StaticStruct()->GetProperty(FName("Asset"));
            if (AssetProp == nullptr)
            {
                return false;   // renamed member; nothing sane to draw
            }

            AssetPicker = FCObjectPropertyCustomization::MakeInstance();
            AssetHandle = MakeShared<FPropertyHandle>(&Value, AssetProp);
        }

        // UpdateAndDraw leaves the write-back to the caller, so commit it here when it reports a change.
        const EPropertyChangeOp Op = AssetPicker->UpdateAndDraw(AssetHandle, FPropertyDrawArgs{});
        if (Op != EPropertyChangeOp::None)
        {
            AssetPicker->UpdatePropertyValue(AssetHandle);
            return true;
        }
        return false;
    }

    EPropertyChangeOp FCurvePropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        bDirty = false;

        const bool bAssetMode = Value.IsUsingAsset();
        const SKeyedCurve& Shown = Value.Resolve();

        ImGui::PushID(this);

        // The thumbnail is the clearest affordance and keeps the collapsed row to one line.
        const ImVec2 ThumbSize(ImGui::GetContentRegionAvail().x - 90.0f, ImGui::GetFrameHeight() * 1.6f);
        const ImVec2 ThumbMin = ImGui::GetCursorScreenPos();
        DrawThumbnail(Shown, ThumbSize);
        if (ImGui::IsMouseHoveringRect(ThumbMin, ImVec2(ThumbMin.x + ThumbSize.x, ThumbMin.y + ThumbSize.y))
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            bEditorOpen = true;
            ImGui::OpenPopup("CurveEditorPopup");
        }
        ImGuiX::TextTooltip("{}", "Click to edit");

        ImGui::SameLine();
        bool bUseAsset = Value.bUseAsset;
        if (ImGui::Checkbox("Asset", &bUseAsset))
        {
            Value.bUseAsset = bUseAsset;
            bDirty = true;
        }
        ImGuiX::TextTooltip("{}", "Use a shared Curve asset instead of the curve authored here. "
                                  "The inline curve is kept, so turning this back off restores it.");

        if (Value.bUseAsset && DrawAssetSlot())
        {
            bDirty = true;
        }

        if (ImGui::BeginPopup("CurveEditorPopup"))
        {
            if (bAssetMode)
            {
                // Read-only, since this curve is owned by another asset and shared with every other user.
                ImGui::TextDisabled("Read-only -- open the Curve asset to edit it.");
                ImGui::Separator();
                DrawThumbnail(Shown, ImVec2(520.0f, 260.0f));
            }
            else
            {
                Editor.SetCurve(&Value.Curve);
                Editor.SetOnModified([this] { bDirty = true; });
                ImGui::BeginChild("##CurveCanvas", ImVec2(560.0f, 320.0f));
                Editor.Draw("InlineCurve");
                ImGui::EndChild();
            }
            ImGui::EndPopup();
        }
        else
        {
            bEditorOpen = false;
            // Dropped on close so a later HandleExternalUpdate replacing Value cannot leave it dangling.
            Editor.SetCurve(nullptr);
        }

        ImGui::PopID();

        // Editing inside the popup is one session, so a drag in the curve editor commits once on close.
        return EditSession.Advance(bDirty, bEditorOpen);
    }

    void FCurvePropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = Value;
        Property->SetValue(CachedValue);
    }

    void FCurvePropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        SCurve Actual;
        Property->GetValue(&Actual);

        // The widget holds a pointer into Value.Curve, so a swap would dangle and discard the edit.
        if (bEditorOpen)
        {
            return;
        }

        if (CachedValue.bUseAsset != Actual.bUseAsset
            || CachedValue.Asset.Get() != Actual.Asset.Get()
            || CachedValue.Curve.Keys.size() != Actual.Curve.Keys.size())
        {
            CachedValue = Value = Actual;
        }
    }

    //~ SGradient ==================================================================================

    void FGradientPropertyCustomization::DrawRamp(const ImVec2& Min, const ImVec2& Max) const
    {
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawCheckerboard(DrawList, Min, Max);

        if (Value.IsEmpty())
        {
            DrawList->AddRect(Min, Max, IM_COL32(70, 70, 78, 255));
            return;
        }

        float TimeMin = 0.0f, TimeMax = 1.0f;
        Value.GetTimeRange(TimeMin, TimeMax);
        if (TimeMax - TimeMin < 1e-6f)
        {
            TimeMax = TimeMin + 1.0f;
        }

        // One quad per pixel column, so any interpolation the evaluator does is handled for free.
        const int32 Columns = Math::Max(1, (int32)(Max.x - Min.x));
        for (int32 i = 0; i < Columns; ++i)
        {
            const float A0 = (float)i / (float)Columns;
            const float A1 = (float)(i + 1) / (float)Columns;
            const ImU32 Col = ToColor(Value.Evaluate(TimeMin + (TimeMax - TimeMin) * A0));
            DrawList->AddRectFilled(
                ImVec2(Min.x + (Max.x - Min.x) * A0, Min.y),
                ImVec2(Min.x + (Max.x - Min.x) * A1, Max.y), Col);
        }
        DrawList->AddRect(Min, Max, IM_COL32(70, 70, 78, 255));
    }

    bool FGradientPropertyCustomization::DrawStops(const ImVec2& RampMin, const ImVec2& RampMax)
    {
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        bool bChanged = false;

        float TimeMin = 0.0f, TimeMax = 1.0f;
        Value.GetTimeRange(TimeMin, TimeMax);
        if (TimeMax - TimeMin < 1e-6f)
        {
            TimeMax = TimeMin + 1.0f;
        }

        const float HandleY = RampMax.y + kStopHandleHalf;
        auto TimeToX = [&](float T)
        {
            return RampMin.x + (RampMax.x - RampMin.x) * ((T - TimeMin) / (TimeMax - TimeMin));
        };

        for (int32 i = 0; i < Value.NumKeys(); ++i)
        {
            const SGradientKey& Key = Value.Keys[i];
            const float X = TimeToX(Key.Time);

            ImGui::SetCursorScreenPos(ImVec2(X - kStopHandleHalf, RampMax.y));
            ImGui::PushID(i);
            ImGui::InvisibleButton("##Stop", ImVec2(kStopHandleHalf * 2.0f, kStopHandleHalf * 2.0f));
            const bool bActive = ImGui::IsItemActive();
            if (ImGui::IsItemClicked())
            {
                SelectedStop = i;
            }

            if (bActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const float NewAlpha = Math::Clamp((ImGui::GetIO().MousePos.x - RampMin.x) / (RampMax.x - RampMin.x), 0.0f, 1.0f);
                Value.Keys[i].Time = TimeMin + (TimeMax - TimeMin) * NewAlpha;
                // Re-sorting mid-drag changes indices, so the selection is re-found by identity afterwards.
                const SGradientKey Moved = Value.Keys[i];
                Value.SortKeys();
                for (int32 j = 0; j < Value.NumKeys(); ++j)
                {
                    if (Value.Keys[j].Time == Moved.Time && Value.Keys[j].Color == Moved.Color)
                    {
                        SelectedStop = j;
                        break;
                    }
                }
                bChanged = true;
            }
            ImGui::PopID();

            const ImU32 Fill = ToColor(Key.Color);
            const ImU32 Edge = (i == SelectedStop) ? IM_COL32(255, 220, 120, 255) : IM_COL32(30, 30, 34, 255);
            DrawList->AddTriangleFilled(
                ImVec2(X, RampMax.y),
                ImVec2(X - kStopHandleHalf, HandleY + kStopHandleHalf),
                ImVec2(X + kStopHandleHalf, HandleY + kStopHandleHalf), Fill);
            DrawList->AddTriangle(
                ImVec2(X, RampMax.y),
                ImVec2(X - kStopHandleHalf, HandleY + kStopHandleHalf),
                ImVec2(X + kStopHandleHalf, HandleY + kStopHandleHalf), Edge, 1.5f);
        }

        return bChanged;
    }

    EPropertyChangeOp FGradientPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        bDirty = false;
        ImGui::PushID(this);

        const float Width  = Math::Max(ImGui::GetContentRegionAvail().x - 40.0f, 60.0f);
        const float Height = ImGui::GetFrameHeight();
        const ImVec2 RampMin = ImGui::GetCursorScreenPos();
        const ImVec2 RampMax(RampMin.x + Width, RampMin.y + Height);

        DrawRamp(RampMin, RampMax);
        ImGui::InvisibleButton("##Ramp", ImVec2(Width, Height));
        if (ImGui::IsItemClicked())
        {
            ImGui::OpenPopup("GradientEditorPopup");
        }
        ImGuiX::TextTooltip("{}", "Click to edit stops");

        ImGui::SameLine();
        ImGui::TextDisabled("%d", Value.NumKeys());

        const bool bPopupOpen = ImGui::BeginPopup("GradientEditorPopup");
        if (bPopupOpen)
        {
            constexpr float EditorWidth = 420.0f;
            const ImVec2 PopupRampMin = ImGui::GetCursorScreenPos();
            const ImVec2 PopupRampMax(PopupRampMin.x + EditorWidth, PopupRampMin.y + 28.0f);

            DrawRamp(PopupRampMin, PopupRampMax);
            ImGui::InvisibleButton("##PopupRamp", ImVec2(EditorWidth, 28.0f));

            // Seeded with the color already there so the ramp does not jump when a stop is added.
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                float TimeMin = 0.0f, TimeMax = 1.0f;
                Value.GetTimeRange(TimeMin, TimeMax);
                if (TimeMax - TimeMin < 1e-6f) { TimeMax = TimeMin + 1.0f; }

                const float Alpha = Math::Clamp((ImGui::GetIO().MousePos.x - PopupRampMin.x) / EditorWidth, 0.0f, 1.0f);
                const float Time  = TimeMin + (TimeMax - TimeMin) * Alpha;
                SelectedStop = Value.AddKey(Time, Value.Evaluate(Time));
                bDirty = true;
            }

            if (DrawStops(PopupRampMin, PopupRampMax))
            {
                bDirty = true;
            }

            ImGui::Dummy(ImVec2(EditorWidth, kStopHandleHalf * 2.0f + 4.0f));
            ImGui::Separator();

            if (SelectedStop >= 0 && SelectedStop < Value.NumKeys())
            {
                SGradientKey& Key = Value.Keys[SelectedStop];

                float Color[4] = { Key.Color.x, Key.Color.y, Key.Color.z, Key.Color.w };
                ImGui::SetNextItemWidth(EditorWidth);
                if (ImGui::ColorEdit4("##StopColor", Color, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf))
                {
                    Key.Color = FVector4(Color[0], Color[1], Color[2], Color[3]);
                    bDirty = true;
                }

                float Time = Key.Time;
                ImGui::SetNextItemWidth(EditorWidth);
                if (ImGui::DragFloat("##StopTime", &Time, 0.005f, 0.0f, 0.0f, "Time %.3f"))
                {
                    Key.Time = Time;
                    Value.SortKeys();
                    bDirty = true;
                }

                ImGui::BeginDisabled(Value.NumKeys() <= 1);
                if (ImGui::Button(LE_ICON_DELETE " Remove Stop"))
                {
                    Value.RemoveKey(SelectedStop);
                    SelectedStop = INDEX_NONE;
                    bDirty = true;
                }
                ImGui::EndDisabled();
            }
            else
            {
                ImGui::TextDisabled("Double-click the ramp to add a stop; click a stop to edit it.");
            }

            ImGui::EndPopup();
        }

        ImGui::PopID();

        // Every stop mutation happens inside the popup, so that whole editing session commits once on close.
        return EditSession.Advance(bDirty, bPopupOpen);
    }

    void FGradientPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = Value;
        Property->SetValue(CachedValue);
    }

    void FGradientPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        SGradient Actual;
        Property->GetValue(&Actual);

        if (CachedValue.Keys.size() != Actual.Keys.size())
        {
            CachedValue = Value = Actual;
            SelectedStop = INDEX_NONE;
        }
    }
}
