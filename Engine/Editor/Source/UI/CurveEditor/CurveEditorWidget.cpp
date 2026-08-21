#include "CurveEditorWidget.h"

#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace Lumina
{
    namespace
    {
        constexpr float KeyHalfSize = 4.5f;
        constexpr float KeyHitRadius = 7.0f;
        constexpr float TangentHandleLength = 46.0f;
        constexpr float TangentHitRadius = 6.0f;
        constexpr float MinViewSpan = 1e-4f;
        constexpr float MaxViewSpan = 1e7f;

        const ImU32 ColBackground = IM_COL32(22, 22, 26, 255);
        const ImU32 ColGridMinor = IM_COL32(255, 255, 255, 12);
        const ImU32 ColGridMajor = IM_COL32(255, 255, 255, 30);
        const ImU32 ColAxis = IM_COL32(120, 180, 240, 90);
        const ImU32 ColLabel = IM_COL32(170, 172, 182, 200);
        const ImU32 ColCurve = IM_COL32(255, 190, 70, 255);
        const ImU32 ColCurveExtrap = IM_COL32(255, 190, 70, 90);
        const ImU32 ColKey = IM_COL32(235, 236, 242, 255);
        const ImU32 ColKeyHovered = IM_COL32(255, 255, 255, 255);
        const ImU32 ColKeySelected = IM_COL32(255, 220, 60, 255);
        const ImU32 ColKeyOutline = IM_COL32(20, 20, 24, 255);
        const ImU32 ColTangent = IM_COL32(120, 200, 255, 200);
        const ImU32 ColTangentBroken = IM_COL32(255, 130, 120, 220);
        const ImU32 ColTimeMarker = IM_COL32(255, 220, 60, 230);
        const ImU32 ColBoxSelect = IM_COL32(120, 180, 240, 40);
        const ImU32 ColBoxSelectBorder = IM_COL32(140, 200, 255, 180);

        // Rounds a rough step up to the nearest 1/2/5 * 10^N so labels stay readable at any zoom.
        float NiceStep(float RoughStep)
        {
            const float Safe = Math::Max(RoughStep, 1e-9f);
            const float Exponent = Math::Floor(std::log10(Safe));
            const float Base = std::pow(10.0f, Exponent);
            const float Normalized = Safe / Base;

            float Multiplier = 1.0f;
            if (Normalized > 5.0f)
            {
                Multiplier = 10.0f;
            }
            else if (Normalized > 2.0f)
            {
                Multiplier = 5.0f;
            }
            else if (Normalized > 1.0f)
            {
                Multiplier = 2.0f;
            }

            return Base * Multiplier;
        }

        int32 DecimalsForStep(float Step)
        {
            if (Step >= 1.0f)
            {
                return 0;
            }

            const int32 Digits = (int32)Math::Ceil(-std::log10(Math::Max(Step, 1e-9f)));
            return Math::Clamp(Digits, 0, 6);
        }

        const char* InterpModeLabel(ECurveInterpMode Mode)
        {
            switch (Mode)
            {
            case ECurveInterpMode::Constant:  return "Constant";
            case ECurveInterpMode::Linear:    return "Linear";
            case ECurveInterpMode::Cubic:     return "Cubic (Auto)";
            case ECurveInterpMode::CubicUser: return "Cubic (User)";
            }
            return "Unknown";
        }
    }

    void FCurveEditorWidget::SetCurve(SKeyedCurve* InCurve)
    {
        if (Curve == InCurve)
        {
            return;
        }

        Curve = InCurve;
        Selection.clear();
        DragMode = EDragMode::None;
        bViewInitialized = false;
    }

    float FCurveEditorWidget::GetPixelsPerTime() const
    {
        return (CanvasMax.x - CanvasMin.x) / Math::Max(ViewMaxTime - ViewMinTime, MinViewSpan);
    }

    float FCurveEditorWidget::GetPixelsPerValue() const
    {
        return (CanvasMax.y - CanvasMin.y) / Math::Max(ViewMaxValue - ViewMinValue, MinViewSpan);
    }

    ImVec2 FCurveEditorWidget::CurveToScreen(float InTime, float InValue) const
    {
        return ImVec2(CanvasMin.x + (InTime - ViewMinTime) * GetPixelsPerTime(),
                      CanvasMax.y - (InValue - ViewMinValue) * GetPixelsPerValue());
    }

    ImVec2 FCurveEditorWidget::ScreenToCurve(const ImVec2& InScreen) const
    {
        return ImVec2(ViewMinTime + (InScreen.x - CanvasMin.x) / GetPixelsPerTime(),
                      ViewMinValue + (CanvasMax.y - InScreen.y) / GetPixelsPerValue());
    }

    ImVec2 FCurveEditorWidget::GetTangentHandlePos(int32 Index, bool bLeave) const
    {
        const SCurveKey& Key = Curve->Keys[Index];
        const float Slope = bLeave ? Key.LeaveTangent : Key.ArriveTangent;

        ImVec2 Direction = ImVec2(GetPixelsPerTime(), -Slope * GetPixelsPerValue());
        const float Length = Math::Max(std::sqrt(Direction.x * Direction.x + Direction.y * Direction.y), 1e-6f);
        Direction = ImVec2(Direction.x / Length, Direction.y / Length);
        if (!bLeave)
        {
            Direction = ImVec2(-Direction.x, -Direction.y);
        }

        const ImVec2 KeyPos = CurveToScreen(Key.Time, Key.Value);
        return ImVec2(KeyPos.x + Direction.x * TangentHandleLength, KeyPos.y + Direction.y * TangentHandleLength);
    }

    void FCurveEditorWidget::NotifyModified()
    {
        bDirtyThisFrame = true;
    }

    void FCurveEditorWidget::ClampView()
    {
        if (ViewMaxTime - ViewMinTime < MinViewSpan)
        {
            const float Center = (ViewMaxTime + ViewMinTime) * 0.5f;
            ViewMinTime = Center - MinViewSpan * 0.5f;
            ViewMaxTime = Center + MinViewSpan * 0.5f;
        }
        if (ViewMaxTime - ViewMinTime > MaxViewSpan)
        {
            const float Center = (ViewMaxTime + ViewMinTime) * 0.5f;
            ViewMinTime = Center - MaxViewSpan * 0.5f;
            ViewMaxTime = Center + MaxViewSpan * 0.5f;
        }

        if (ViewMaxValue - ViewMinValue < MinViewSpan)
        {
            const float Center = (ViewMaxValue + ViewMinValue) * 0.5f;
            ViewMinValue = Center - MinViewSpan * 0.5f;
            ViewMaxValue = Center + MinViewSpan * 0.5f;
        }
        if (ViewMaxValue - ViewMinValue > MaxViewSpan)
        {
            const float Center = (ViewMaxValue + ViewMinValue) * 0.5f;
            ViewMinValue = Center - MaxViewSpan * 0.5f;
            ViewMaxValue = Center + MaxViewSpan * 0.5f;
        }
    }

    void FCurveEditorWidget::ZoomView(const ImVec2& Anchor, float Scale, bool bZoomX, bool bZoomY)
    {
        if (bZoomX)
        {
            ViewMinTime = Anchor.x + (ViewMinTime - Anchor.x) * Scale;
            ViewMaxTime = Anchor.x + (ViewMaxTime - Anchor.x) * Scale;
        }
        if (bZoomY)
        {
            ViewMinValue = Anchor.y + (ViewMinValue - Anchor.y) * Scale;
            ViewMaxValue = Anchor.y + (ViewMaxValue - Anchor.y) * Scale;
        }

        ClampView();
    }

    void FCurveEditorWidget::FrameRange(float MinTime, float MaxTime, float MinValue, float MaxValue)
    {
        float TimeSpan = MaxTime - MinTime;
        float ValueSpan = MaxValue - MinValue;

        if (TimeSpan < MinViewSpan)
        {
            TimeSpan = 1.0f;
            const float Center = (MaxTime + MinTime) * 0.5f;
            MinTime = Center - 0.5f;
            MaxTime = Center + 0.5f;
        }
        if (ValueSpan < MinViewSpan)
        {
            ValueSpan = 1.0f;
            const float Center = (MaxValue + MinValue) * 0.5f;
            MinValue = Center - 0.5f;
            MaxValue = Center + 0.5f;
        }

        const float TimePad = TimeSpan * 0.08f;
        const float ValuePad = ValueSpan * 0.12f;

        ViewMinTime = MinTime - TimePad;
        ViewMaxTime = MaxTime + TimePad;
        ViewMinValue = MinValue - ValuePad;
        ViewMaxValue = MaxValue + ValuePad;

        ClampView();
    }

    void FCurveEditorWidget::FrameAll()
    {
        if (Curve == nullptr || Curve->IsEmpty())
        {
            FrameRange(0.0f, 1.0f, 0.0f, 1.0f);
            return;
        }

        float MinTime, MaxTime, MinValue, MaxValue;
        Curve->GetTimeRange(MinTime, MaxTime);
        Curve->GetValueRange(MinValue, MaxValue);
        FrameRange(MinTime, MaxTime, MinValue, MaxValue);
    }

    void FCurveEditorWidget::FrameSelection()
    {
        if (Curve == nullptr || Selection.empty())
        {
            FrameAll();
            return;
        }

        float MinTime = FLT_MAX;
        float MaxTime = -FLT_MAX;
        float MinValue = FLT_MAX;
        float MaxValue = -FLT_MAX;

        for (int32 Index : Selection)
        {
            if (Index < 0 || Index >= Curve->NumKeys())
            {
                continue;
            }

            const SCurveKey& Key = Curve->Keys[Index];
            MinTime = Math::Min(MinTime, Key.Time);
            MaxTime = Math::Max(MaxTime, Key.Time);
            MinValue = Math::Min(MinValue, Key.Value);
            MaxValue = Math::Max(MaxValue, Key.Value);
        }

        if (MinTime > MaxTime)
        {
            FrameAll();
            return;
        }

        FrameRange(MinTime, MaxTime, MinValue, MaxValue);
    }

    bool FCurveEditorWidget::IsSelected(int32 Index) const
    {
        for (int32 Selected : Selection)
        {
            if (Selected == Index)
            {
                return true;
            }
        }
        return false;
    }

    void FCurveEditorWidget::ToggleSelection(int32 Index)
    {
        for (auto It = Selection.begin(); It != Selection.end(); ++It)
        {
            if (*It == Index)
            {
                Selection.erase(It);
                return;
            }
        }

        Selection.push_back(Index);
    }

    void FCurveEditorWidget::SetSelection(int32 Index)
    {
        Selection.clear();
        Selection.push_back(Index);
    }

    void FCurveEditorWidget::SelectNone()
    {
        Selection.clear();
    }

    void FCurveEditorWidget::BeginKeyDrag()
    {
        DragStartTimes.clear();
        DragStartValues.clear();
        DragStartTimes.reserve(Selection.size());
        DragStartValues.reserve(Selection.size());

        for (int32 Index : Selection)
        {
            DragStartTimes.push_back(Curve->Keys[Index].Time);
            DragStartValues.push_back(Curve->Keys[Index].Value);
        }
    }

    // Rewrites the selection and tangent drag target through the permutation so they follow their keys.
    void FCurveEditorWidget::ResortAndRemapSelection()
    {
        const int32 Num = Curve->NumKeys();
        if (Num < 2)
        {
            return;
        }

        bool bSorted = true;
        for (int32 Index = 1; Index < Num; ++Index)
        {
            if (Curve->Keys[Index].Time < Curve->Keys[Index - 1].Time)
            {
                bSorted = false;
                break;
            }
        }

        if (bSorted)
        {
            return;
        }

        TVector<int32> Order;
        Order.reserve(Num);
        for (int32 Index = 0; Index < Num; ++Index)
        {
            Order.push_back(Index);
        }

        Algo::StableSort(Order.begin(), Order.end(), [this](int32 A, int32 B)
        {
            return Curve->Keys[A].Time < Curve->Keys[B].Time;
        });

        TVector<SCurveKey> Sorted;
        TVector<int32> Remap;
        Sorted.reserve(Num);
        Remap.resize(Num);

        for (int32 Index = 0; Index < Num; ++Index)
        {
            Sorted.push_back(Curve->Keys[Order[Index]]);
            Remap[Order[Index]] = Index;
        }

        Curve->Keys = Sorted;

        for (int32& Selected : Selection)
        {
            Selected = Remap[Selected];
        }

        if (TangentDragKey != INDEX_NONE)
        {
            TangentDragKey = Remap[TangentDragKey];
        }
    }

    float FCurveEditorWidget::SnapTime(float InTime) const
    {
        if (!bSnapTime || TimeSnapStep <= 0.0f)
        {
            return InTime;
        }
        return Math::Round(InTime / TimeSnapStep) * TimeSnapStep;
    }

    float FCurveEditorWidget::SnapValue(float InValue) const
    {
        if (!bSnapValue || ValueSnapStep <= 0.0f)
        {
            return InValue;
        }
        return Math::Round(InValue / ValueSnapStep) * ValueSnapStep;
    }

    int32 FCurveEditorWidget::AddKeyAt(float InTime, float InValue)
    {
        const int32 Index = Curve->AddKey(SnapTime(InTime), SnapValue(InValue));
        Curve->ComputeAutoTangents();
        SetSelection(Index);
        NotifyModified();
        return Index;
    }

    void FCurveEditorWidget::DeleteSelected()
    {
        if (Selection.empty())
        {
            return;
        }

        TVector<int32> Sorted = Selection;
        Algo::Sort(Sorted.begin(), Sorted.end());

        for (int32 Index = (int32)Sorted.size() - 1; Index >= 0; --Index)
        {
            Curve->RemoveKey(Sorted[Index]);
        }

        Selection.clear();
        Curve->ComputeAutoTangents();
        NotifyModified();
    }

    void FCurveEditorWidget::SetSelectionInterpMode(ECurveInterpMode Mode, bool bBroken)
    {
        if (Selection.empty())
        {
            return;
        }

        for (int32 Index : Selection)
        {
            if (Index < 0 || Index >= Curve->NumKeys())
            {
                continue;
            }

            SCurveKey& Key = Curve->Keys[Index];
            Key.InterpMode = Mode;
            Key.bTangentsBroken = bBroken && Mode == ECurveInterpMode::CubicUser;
            if (!Key.bTangentsBroken && Mode == ECurveInterpMode::CubicUser)
            {
                Key.ArriveTangent = Key.LeaveTangent;
            }
        }

        Curve->ComputeAutoTangents();
        NotifyModified();
    }

    void FCurveEditorWidget::FlattenSelectedTangents()
    {
        for (int32 Index : Selection)
        {
            if (Index < 0 || Index >= Curve->NumKeys())
            {
                continue;
            }

            SCurveKey& Key = Curve->Keys[Index];
            if (Key.InterpMode == ECurveInterpMode::Cubic)
            {
                Key.InterpMode = ECurveInterpMode::CubicUser;
            }

            Key.ArriveTangent = 0.0f;
            Key.LeaveTangent = 0.0f;
        }

        NotifyModified();
    }

    void FCurveEditorWidget::Draw(const char* ID)
    {
        if (Curve == nullptr)
        {
            ImGui::TextDisabled("No curve bound.");
            return;
        }

        bDirtyThisFrame = false;

        ImGui::PushID(ID);

        DrawToolbar();

        ImGui::BeginChild("CurveCanvas", ImVec2(0.0f, 0.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);

        CanvasMin = ImGui::GetCursorScreenPos();
        ImVec2 CanvasSize = ImGui::GetContentRegionAvail();
        CanvasSize.x = Math::Max(CanvasSize.x, 64.0f);
        CanvasSize.y = Math::Max(CanvasSize.y, 64.0f);
        CanvasMax = ImVec2(CanvasMin.x + CanvasSize.x, CanvasMin.y + CanvasSize.y);

        if (!bViewInitialized)
        {
            FrameAll();
            bViewInitialized = true;
        }

        ImGui::InvisibleButton("CanvasSurface", CanvasSize,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);

        const bool bHovered = ImGui::IsItemHovered();

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawList->PushClipRect(CanvasMin, CanvasMax, true);
        DrawList->AddRectFilled(CanvasMin, CanvasMax, ColBackground);

        UpdateHover();
        HandleInput(bHovered);

        DrawGrid(DrawList);
        DrawCurve(DrawList);
        DrawTangents(DrawList);
        DrawKeys(DrawList);

        if (DragMode == EDragMode::BoxSelect)
        {
            const ImVec2 Current = ImGui::GetIO().MousePos;
            const ImVec2 Lo = ImVec2(Math::Min(DragStartScreen.x, Current.x), Math::Min(DragStartScreen.y, Current.y));
            const ImVec2 Hi = ImVec2(Math::Max(DragStartScreen.x, Current.x), Math::Max(DragStartScreen.y, Current.y));
            DrawList->AddRectFilled(Lo, Hi, ColBoxSelect);
            DrawList->AddRect(Lo, Hi, ColBoxSelectBorder);
        }

        DrawOverlay(DrawList);

        DrawList->PopClipRect();

        DrawContextMenu();

        ImGui::EndChild();
        ImGui::PopID();

        if (bDirtyThisFrame && OnModified)
        {
            OnModified();
        }
    }

    void FCurveEditorWidget::DrawToolbar()
    {
        const bool bHasSelection = !Selection.empty();

        if (ImGui::Button(LE_ICON_STAIRS "##Constant"))
        {
            SetSelectionInterpMode(ECurveInterpMode::Constant, false);
        }
        ImGuiX::TextTooltip("Constant");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_VECTOR_POLYLINE "##Linear"))
        {
            SetSelectionInterpMode(ECurveInterpMode::Linear, false);
        }
        ImGuiX::TextTooltip("Linear");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_VECTOR_CURVE "##Cubic"))
        {
            SetSelectionInterpMode(ECurveInterpMode::Cubic, false);
        }
        ImGuiX::TextTooltip("Cubic (auto tangents)");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_VECTOR_RADIUS "##CubicUser"))
        {
            SetSelectionInterpMode(ECurveInterpMode::CubicUser, false);
        }
        ImGuiX::TextTooltip("Cubic (user tangents)");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_ARROW_EXPAND_ALL "##Frame"))
        {
            bHasSelection ? FrameSelection() : FrameAll();
        }
        ImGuiX::TextTooltip("Frame selection or all keys (F)");

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        ImGui::Checkbox("Snap T", &bSnapTime);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::DragFloat("##TimeSnap", &TimeSnapStep, 0.005f, 0.0001f, 1000.0f, "%.3f");

        ImGui::SameLine();
        ImGui::Checkbox("Snap V", &bSnapValue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::DragFloat("##ValueSnap", &ValueSnapStep, 0.005f, 0.0001f, 1000.0f, "%.3f");

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Numeric editing of the last-selected key.
        const int32 ActiveKey = bHasSelection ? Selection.back() : INDEX_NONE;
        if (ActiveKey != INDEX_NONE && ActiveKey < Curve->NumKeys())
        {
            SCurveKey& Key = Curve->Keys[ActiveKey];

            float Time = Key.Time;
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragFloat("Time", &Time, 0.01f, 0.0f, 0.0f, "%.3f"))
            {
                Key.Time = Time;
                ResortAndRemapSelection();
                Curve->ComputeAutoTangents();
                NotifyModified();
            }

            ImGui::SameLine();
            float Value = Key.Value;
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragFloat("Value", &Value, 0.01f, 0.0f, 0.0f, "%.3f"))
            {
                Curve->Keys[ActiveKey].Value = Value;
                Curve->ComputeAutoTangents();
                NotifyModified();
            }
        }
        else
        {
            ImGui::TextDisabled("No key selected");
        }
    }

    void FCurveEditorWidget::DrawGrid(ImDrawList* DrawList) const
    {
        const float Width = CanvasMax.x - CanvasMin.x;
        const float Height = CanvasMax.y - CanvasMin.y;

        const float TimeStep = NiceStep((ViewMaxTime - ViewMinTime) * (90.0f / Math::Max(Width, 1.0f)));
        const float ValueStep = NiceStep((ViewMaxValue - ViewMinValue) * (60.0f / Math::Max(Height, 1.0f)));

        char Label[32];

        // Vertical (time) lines.
        {
            const int32 Decimals = DecimalsForStep(TimeStep);
            const float Minor = TimeStep * 0.2f;
            const float FirstMinor = Math::Floor(ViewMinTime / Minor) * Minor;
            for (float T = FirstMinor; T <= ViewMaxTime; T += Minor)
            {
                const float X = CurveToScreen(T, 0.0f).x;
                DrawList->AddLine(ImVec2(X, CanvasMin.y), ImVec2(X, CanvasMax.y), ColGridMinor);
            }

            const float FirstMajor = Math::Floor(ViewMinTime / TimeStep) * TimeStep;
            for (float T = FirstMajor; T <= ViewMaxTime; T += TimeStep)
            {
                const float X = CurveToScreen(T, 0.0f).x;
                const bool bZero = Math::Abs(T) < TimeStep * 0.01f;
                DrawList->AddLine(ImVec2(X, CanvasMin.y), ImVec2(X, CanvasMax.y), bZero ? ColAxis : ColGridMajor, bZero ? 1.5f : 1.0f);

                snprintf(Label, sizeof(Label), "%.*f", Decimals, bZero ? 0.0f : T);
                DrawList->AddText(ImVec2(X + 3.0f, CanvasMax.y - ImGui::GetTextLineHeight() - 2.0f), ColLabel, Label);
            }
        }

        // Horizontal (value) lines.
        {
            const int32 Decimals = DecimalsForStep(ValueStep);
            const float Minor = ValueStep * 0.2f;
            const float FirstMinor = Math::Floor(ViewMinValue / Minor) * Minor;
            for (float V = FirstMinor; V <= ViewMaxValue; V += Minor)
            {
                const float Y = CurveToScreen(0.0f, V).y;
                DrawList->AddLine(ImVec2(CanvasMin.x, Y), ImVec2(CanvasMax.x, Y), ColGridMinor);
            }

            const float FirstMajor = Math::Floor(ViewMinValue / ValueStep) * ValueStep;
            for (float V = FirstMajor; V <= ViewMaxValue; V += ValueStep)
            {
                const float Y = CurveToScreen(0.0f, V).y;
                const bool bZero = Math::Abs(V) < ValueStep * 0.01f;
                DrawList->AddLine(ImVec2(CanvasMin.x, Y), ImVec2(CanvasMax.x, Y), bZero ? ColAxis : ColGridMajor, bZero ? 1.5f : 1.0f);

                snprintf(Label, sizeof(Label), "%.*f", Decimals, bZero ? 0.0f : V);
                DrawList->AddText(ImVec2(CanvasMin.x + 4.0f, Y + 2.0f), ColLabel, Label);
            }
        }
    }

    void FCurveEditorWidget::DrawCurve(ImDrawList* DrawList) const
    {
        const float Width = CanvasMax.x - CanvasMin.x;
        const int32 SampleCount = Math::Clamp((int32)(Width / 2.0f), 32, 2048);

        float KeyedMin = 0.0f;
        float KeyedMax = 0.0f;
        Curve->GetTimeRange(KeyedMin, KeyedMax);
        const bool bHasKeys = !Curve->IsEmpty();

        // Two passes so extrapolated stretches read as dimmer than the keyed range.
        bool bPrevValid = false;
        ImVec2 PrevPoint = ImVec2(0.0f, 0.0f);
        bool bPrevExtrapolated = false;

        for (int32 Sample = 0; Sample <= SampleCount; ++Sample)
        {
            const float Alpha = (float)Sample / (float)SampleCount;
            const float Time = Math::Lerp(ViewMinTime, ViewMaxTime, Alpha);
            const ImVec2 Point = CurveToScreen(Time, Curve->Evaluate(Time));
            const bool bExtrapolated = bHasKeys && (Time < KeyedMin || Time > KeyedMax);

            if (bPrevValid)
            {
                const bool bDim = bExtrapolated || bPrevExtrapolated;
                DrawList->AddLine(PrevPoint, Point, bDim ? ColCurveExtrap : ColCurve, bDim ? 1.5f : 2.0f);
            }

            PrevPoint = Point;
            bPrevExtrapolated = bExtrapolated;
            bPrevValid = true;
        }
    }

    void FCurveEditorWidget::DrawTangents(ImDrawList* DrawList) const
    {
        for (int32 Index : Selection)
        {
            if (Index < 0 || Index >= Curve->NumKeys())
            {
                continue;
            }

            const SCurveKey& Key = Curve->Keys[Index];
            if (!Key.IsCubic())
            {
                continue;
            }

            const ImVec2 KeyPos = CurveToScreen(Key.Time, Key.Value);
            const ImU32 Color = Key.bTangentsBroken ? ColTangentBroken : ColTangent;

            for (int32 Side = 0; Side < 2; ++Side)
            {
                const bool bLeave = Side == 1;
                if ((bLeave && Index == Curve->NumKeys() - 1) || (!bLeave && Index == 0))
                {
                    continue;
                }

                const ImVec2 Handle = GetTangentHandlePos(Index, bLeave);
                const bool bHot = HoveredTangentKey == Index && bHoveredTangentLeave == bLeave;

                DrawList->AddLine(KeyPos, Handle, Color, 1.5f);
                DrawList->AddCircleFilled(Handle, bHot ? 5.0f : 3.5f, Color);
                DrawList->AddCircle(Handle, bHot ? 5.0f : 3.5f, ColKeyOutline);
            }
        }
    }

    void FCurveEditorWidget::DrawKeys(ImDrawList* DrawList) const
    {
        for (int32 Index = 0; Index < Curve->NumKeys(); ++Index)
        {
            const SCurveKey& Key = Curve->Keys[Index];
            const ImVec2 Pos = CurveToScreen(Key.Time, Key.Value);
            const bool bSelected = IsSelected(Index);
            const bool bHovered = HoveredKey == Index;
            const ImU32 Color = bSelected ? ColKeySelected : (bHovered ? ColKeyHovered : ColKey);
            const float Half = bSelected || bHovered ? KeyHalfSize + 1.0f : KeyHalfSize;

            if (Key.IsCubic())
            {
                // Diamond marks a curved key, square a constant/linear one.
                const ImVec2 Points[4] =
                {
                    ImVec2(Pos.x, Pos.y - Half),
                    ImVec2(Pos.x + Half, Pos.y),
                    ImVec2(Pos.x, Pos.y + Half),
                    ImVec2(Pos.x - Half, Pos.y),
                };
                DrawList->AddQuadFilled(Points[0], Points[1], Points[2], Points[3], Color);
                DrawList->AddQuad(Points[0], Points[1], Points[2], Points[3], ColKeyOutline);
            }
            else
            {
                DrawList->AddRectFilled(ImVec2(Pos.x - Half, Pos.y - Half), ImVec2(Pos.x + Half, Pos.y + Half), Color);
                DrawList->AddRect(ImVec2(Pos.x - Half, Pos.y - Half), ImVec2(Pos.x + Half, Pos.y + Half), ColKeyOutline);
            }
        }
    }

    void FCurveEditorWidget::DrawOverlay(ImDrawList* DrawList) const
    {
        if (bShowTimeMarker)
        {
            const float X = CurveToScreen(TimeMarker, 0.0f).x;
            if (X >= CanvasMin.x && X <= CanvasMax.x)
            {
                DrawList->AddLine(ImVec2(X, CanvasMin.y), ImVec2(X, CanvasMax.y), ColTimeMarker, 1.5f);
                DrawList->AddTriangleFilled(ImVec2(X - 5.0f, CanvasMin.y), ImVec2(X + 5.0f, CanvasMin.y),
                                            ImVec2(X, CanvasMin.y + 7.0f), ColTimeMarker);
            }
        }

        char Buffer[96];
        snprintf(Buffer, sizeof(Buffer), "T %.3f   V %.3f   (%d keys, %d selected)",
            MouseCurvePos.x, MouseCurvePos.y, Curve->NumKeys(), (int32)Selection.size());

        const ImVec2 TextSize = ImGui::CalcTextSize(Buffer);
        const ImVec2 Origin = ImVec2(CanvasMax.x - TextSize.x - 10.0f, CanvasMin.y + 6.0f);

        DrawList->AddRectFilled(ImVec2(Origin.x - 5.0f, Origin.y - 3.0f),
                                ImVec2(Origin.x + TextSize.x + 5.0f, Origin.y + TextSize.y + 3.0f),
                                IM_COL32(0, 0, 0, 120), 3.0f);
        DrawList->AddText(Origin, ColLabel, Buffer);
    }

    void FCurveEditorWidget::UpdateHover()
    {
        HoveredKey = INDEX_NONE;
        HoveredTangentKey = INDEX_NONE;

        if (DragMode != EDragMode::None)
        {
            return;
        }

        const ImVec2 Mouse = ImGui::GetIO().MousePos;

        // Tangent grabbers win over keys, since they sit further from the curve and are easier to miss.
        for (int32 Index : Selection)
        {
            if (Index < 0 || Index >= Curve->NumKeys() || !Curve->Keys[Index].IsCubic())
            {
                continue;
            }

            for (int32 Side = 0; Side < 2; ++Side)
            {
                const bool bLeave = Side == 1;
                if ((bLeave && Index == Curve->NumKeys() - 1) || (!bLeave && Index == 0))
                {
                    continue;
                }

                const ImVec2 Handle = GetTangentHandlePos(Index, bLeave);
                if (Math::Abs(Mouse.x - Handle.x) <= TangentHitRadius && Math::Abs(Mouse.y - Handle.y) <= TangentHitRadius)
                {
                    HoveredTangentKey = Index;
                    bHoveredTangentLeave = bLeave;
                }
            }
        }

        if (HoveredTangentKey != INDEX_NONE)
        {
            return;
        }

        float BestDistance = KeyHitRadius;
        for (int32 Index = 0; Index < Curve->NumKeys(); ++Index)
        {
            const ImVec2 Pos = CurveToScreen(Curve->Keys[Index].Time, Curve->Keys[Index].Value);
            const float Distance = Math::Max(Math::Abs(Mouse.x - Pos.x), Math::Abs(Mouse.y - Pos.y));
            if (Distance <= BestDistance)
            {
                BestDistance = Distance;
                HoveredKey = Index;
            }
        }
    }

    void FCurveEditorWidget::UpdateKeyDrag()
    {
        const ImVec2 Mouse = ImGui::GetIO().MousePos;
        const float DeltaTime = (Mouse.x - DragStartScreen.x) / GetPixelsPerTime();
        const float DeltaValue = -(Mouse.y - DragStartScreen.y) / GetPixelsPerValue();

        for (SIZE_T Slot = 0; Slot < Selection.size(); ++Slot)
        {
            const int32 Index = Selection[Slot];
            if (Index < 0 || Index >= Curve->NumKeys())
            {
                continue;
            }

            SCurveKey& Key = Curve->Keys[Index];
            Key.Time = SnapTime(DragStartTimes[Slot] + DeltaTime);
            Key.Value = SnapValue(DragStartValues[Slot] + DeltaValue);
        }

        ResortAndRemapSelection();
        Curve->ComputeAutoTangents();
        NotifyModified();
    }

    void FCurveEditorWidget::UpdateTangentDrag()
    {
        if (TangentDragKey < 0 || TangentDragKey >= Curve->NumKeys())
        {
            return;
        }

        SCurveKey& Key = Curve->Keys[TangentDragKey];
        const ImVec2 KeyPos = CurveToScreen(Key.Time, Key.Value);
        const ImVec2 Mouse = ImGui::GetIO().MousePos;

        float DeltaX = Mouse.x - KeyPos.x;
        float DeltaY = Mouse.y - KeyPos.y;
        if (!bTangentDragLeave)
        {
            DeltaX = -DeltaX;
            DeltaY = -DeltaY;
        }

        DeltaX = Math::Max(DeltaX, 2.0f);

        const float Slope = (-DeltaY / GetPixelsPerValue()) / (DeltaX / GetPixelsPerTime());

        // Editing a handle always promotes the key to user tangents; auto would overwrite it.
        Key.InterpMode = ECurveInterpMode::CubicUser;

        if (Key.bTangentsBroken)
        {
            (bTangentDragLeave ? Key.LeaveTangent : Key.ArriveTangent) = Slope;
        }
        else
        {
            Key.ArriveTangent = Slope;
            Key.LeaveTangent = Slope;
        }

        NotifyModified();
    }

    void FCurveEditorWidget::HandleInput(bool bHovered)
    {
        const ImGuiIO& IO = ImGui::GetIO();
        MouseCurvePos = ScreenToCurve(IO.MousePos);

        if (bHovered && IO.MouseWheel != 0.0f)
        {
            bool bZoomX = true;
            bool bZoomY = true;
            if (IO.KeyCtrl && !IO.KeyShift)
            {
                bZoomY = false;
            }
            else if (IO.KeyShift && !IO.KeyCtrl)
            {
                bZoomX = false;
            }

            const float Scale = IO.MouseWheel > 0.0f ? 1.0f / 1.15f : 1.15f;
            ZoomView(ScreenToCurve(IO.MousePos), Scale, bZoomX, bZoomY);
        }

        // Drag start.
        if (DragMode == EDragMode::None && bHovered)
        {
            const bool bPanChord = ImGui::IsMouseClicked(ImGuiMouseButton_Middle)
                || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && IO.KeyAlt && HoveredTangentKey == INDEX_NONE);

            if (bPanChord)
            {
                DragMode = EDragMode::Pan;
                DragStartScreen = IO.MousePos;
                DragStartCurve = ImVec2(ViewMinTime, ViewMinValue);
            }
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                if (HoveredTangentKey != INDEX_NONE)
                {
                    DragMode = EDragMode::Tangent;
                    TangentDragKey = HoveredTangentKey;
                    bTangentDragLeave = bHoveredTangentLeave;
                    DragStartScreen = IO.MousePos;

                    if (IO.KeyAlt)
                    {
                        Curve->Keys[TangentDragKey].bTangentsBroken = true;
                        NotifyModified();
                    }
                }
                else if (HoveredKey != INDEX_NONE)
                {
                    if (IO.KeyCtrl)
                    {
                        ToggleSelection(HoveredKey);
                    }
                    else if (!IsSelected(HoveredKey))
                    {
                        SetSelection(HoveredKey);
                    }

                    if (IsSelected(HoveredKey))
                    {
                        DragMode = EDragMode::Keys;
                        DragStartScreen = IO.MousePos;
                        BeginKeyDrag();
                    }
                }
                else
                {
                    DragMode = EDragMode::BoxSelect;
                    DragStartScreen = IO.MousePos;
                    if (!IO.KeyCtrl)
                    {
                        SelectNone();
                    }
                }

                bDragMoved = false;
            }
        }

        // Drag update.
        switch (DragMode)
        {
        case EDragMode::Pan:
            {
                const float DeltaTime = (IO.MousePos.x - DragStartScreen.x) / GetPixelsPerTime();
                const float DeltaValue = -(IO.MousePos.y - DragStartScreen.y) / GetPixelsPerValue();
                const float TimeSpan = ViewMaxTime - ViewMinTime;
                const float ValueSpan = ViewMaxValue - ViewMinValue;

                ViewMinTime = DragStartCurve.x - DeltaTime;
                ViewMaxTime = ViewMinTime + TimeSpan;
                ViewMinValue = DragStartCurve.y - DeltaValue;
                ViewMaxValue = ViewMinValue + ValueSpan;

                if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    DragMode = EDragMode::None;
                }
                break;
            }

        case EDragMode::Keys:
            {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    if (Math::Abs(IO.MousePos.x - DragStartScreen.x) > 1.0f || Math::Abs(IO.MousePos.y - DragStartScreen.y) > 1.0f)
                    {
                        bDragMoved = true;
                        UpdateKeyDrag();
                    }
                }
                else
                {
                    DragMode = EDragMode::None;
                }
                break;
            }

        case EDragMode::Tangent:
            {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    UpdateTangentDrag();
                }
                else
                {
                    DragMode = EDragMode::None;
                    TangentDragKey = INDEX_NONE;
                }
                break;
            }

        case EDragMode::BoxSelect:
            {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    const ImVec2 Lo = ImVec2(Math::Min(DragStartScreen.x, IO.MousePos.x), Math::Min(DragStartScreen.y, IO.MousePos.y));
                    const ImVec2 Hi = ImVec2(Math::Max(DragStartScreen.x, IO.MousePos.x), Math::Max(DragStartScreen.y, IO.MousePos.y));

                    for (int32 Index = 0; Index < Curve->NumKeys(); ++Index)
                    {
                        const ImVec2 Pos = CurveToScreen(Curve->Keys[Index].Time, Curve->Keys[Index].Value);
                        if (Pos.x >= Lo.x && Pos.x <= Hi.x && Pos.y >= Lo.y && Pos.y <= Hi.y && !IsSelected(Index))
                        {
                            Selection.push_back(Index);
                        }
                    }

                    DragMode = EDragMode::None;
                }
                break;
            }

        case EDragMode::None:
        default:
            break;
        }

        if (!bHovered)
        {
            return;
        }

        // Double-click adds a key; on the curve it lands on the evaluated value.
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && HoveredKey == INDEX_NONE && HoveredTangentKey == INDEX_NONE)
        {
            const float Time = MouseCurvePos.x;
            const ImVec2 OnCurve = CurveToScreen(Time, Curve->Evaluate(Time));
            const bool bOnCurve = Math::Abs(OnCurve.y - IO.MousePos.y) <= 8.0f;

            AddKeyAt(Time, bOnCurve ? Curve->Evaluate(Time) : MouseCurvePos.y);
            DragMode = EDragMode::None;
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            ContextKey = HoveredKey;
            ImGui::OpenPopup("CurveContextMenu");
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            DeleteSelected();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            Selection.empty() ? FrameAll() : FrameSelection();
        }
    }

    void FCurveEditorWidget::DrawContextMenu()
    {
        if (!ImGui::BeginPopup("CurveContextMenu"))
        {
            return;
        }

        if (ContextKey != INDEX_NONE && ContextKey < Curve->NumKeys())
        {
            const SCurveKey& Key = Curve->Keys[ContextKey];
            ImGui::TextDisabled("Key %d - %s", ContextKey, InterpModeLabel(Key.InterpMode));
            ImGui::Separator();

            if (ImGui::RadioButton("Constant", Key.InterpMode == ECurveInterpMode::Constant))
            {
                SetSelectionInterpMode(ECurveInterpMode::Constant, false);
            }
            if (ImGui::RadioButton("Linear", Key.InterpMode == ECurveInterpMode::Linear))
            {
                SetSelectionInterpMode(ECurveInterpMode::Linear, false);
            }
            if (ImGui::RadioButton("Cubic (Auto)", Key.InterpMode == ECurveInterpMode::Cubic))
            {
                SetSelectionInterpMode(ECurveInterpMode::Cubic, false);
            }
            if (ImGui::RadioButton("Cubic (User)", Key.InterpMode == ECurveInterpMode::CubicUser && !Key.bTangentsBroken))
            {
                SetSelectionInterpMode(ECurveInterpMode::CubicUser, false);
            }
            if (ImGui::RadioButton("Cubic (Broken)", Key.InterpMode == ECurveInterpMode::CubicUser && Key.bTangentsBroken))
            {
                SetSelectionInterpMode(ECurveInterpMode::CubicUser, true);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Flatten Tangents"))
            {
                FlattenSelectedTangents();
            }
            if (ImGui::MenuItem("Delete Key", "Del"))
            {
                DeleteSelected();
            }
        }
        else
        {
            if (ImGui::MenuItem("Add Key Here"))
            {
                AddKeyAt(MouseCurvePos.x, MouseCurvePos.y);
            }
            if (ImGui::MenuItem("Frame View", "F"))
            {
                FrameAll();
            }

            ImGui::Separator();

            static const char* ExtrapolationLabels[] = { "Clamp", "Cycle", "Cycle With Offset", "Oscillate", "Linear" };

            if (ImGui::BeginMenu("Pre-Extrapolation"))
            {
                for (int32 Index = 0; Index < IM_ARRAYSIZE(ExtrapolationLabels); ++Index)
                {
                    if (ImGui::RadioButton(ExtrapolationLabels[Index], (int32)Curve->PreExtrapolation == Index))
                    {
                        Curve->PreExtrapolation = (ECurveExtrapolation)Index;
                        NotifyModified();
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Post-Extrapolation"))
            {
                for (int32 Index = 0; Index < IM_ARRAYSIZE(ExtrapolationLabels); ++Index)
                {
                    if (ImGui::RadioButton(ExtrapolationLabels[Index], (int32)Curve->PostExtrapolation == Index))
                    {
                        Curve->PostExtrapolation = (ECurveExtrapolation)Index;
                        NotifyModified();
                    }
                }
                ImGui::EndMenu();
            }
        }

        ImGui::EndPopup();
    }
}
