#include "ProfilerEditorTool.h"
#include "ProfilerViewCommon.h"

#include <cfloat>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "Core/Math/Math.h"
#include "Core/Profiler/GameplayProfiler.h"
#include "Core/UpdateStage.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Systems/SystemAccess.h"
#include "World/WorldManager.h"
#include "World/WorldContext.h"

namespace Lumina
{
    // Internal linkage, since these names would collide with another TU's Detail helpers.
    namespace
    {
        namespace InsightsDetail
        {
            const char* StageName(uint8 Stage)
            {
                return Stage < (uint8)EUpdateStage::Max ? GUpdateStageNames[Stage] : "?";
            }

            // Distinct, stable hue per stage so the shape of a frame reads without parsing labels.
            ImVec4 StageColor(uint8 Stage)
            {
                switch ((EUpdateStage)Stage)
                {
                case EUpdateStage::FrameStart:    return ImVec4(0.36f, 0.58f, 0.86f, 1.0f);
                case EUpdateStage::PrePhysics:    return ImVec4(0.38f, 0.74f, 0.52f, 1.0f);
                case EUpdateStage::DuringPhysics: return ImVec4(0.82f, 0.66f, 0.28f, 1.0f);
                case EUpdateStage::PostPhysics:   return ImVec4(0.77f, 0.47f, 0.36f, 1.0f);
                case EUpdateStage::FrameEnd:      return ImVec4(0.66f, 0.49f, 0.82f, 1.0f);
                case EUpdateStage::Paused:        return ImVec4(0.52f, 0.52f, 0.55f, 1.0f);
                default:                          break;
                }
                return ImVec4(0.47f, 0.47f, 0.50f, 1.0f);
            }

            // Comma-joined display names for a set of access ids (component type ids), resolved via the runtime registry.
            FString AccessList(const TVector<uint32>& Ids)
            {
                FString Out;
                for (uint32 Id : Ids)
                {
                    const FName Name = GetAccessTypeName(Id);
                    if (!Out.empty())
                    {
                        Out += ", ";
                    }
                    if (Name.IsNone())
                    {
                        Out += "<unknown>";
                    }
                    else
                    {
                        Name.AppendString(Out);
                    }
                }
                return Out;
            }

            bool Contains(const TVector<uint32>& Ids, uint32 Id)
            {
                for (uint32 Existing : Ids)
                {
                    if (Existing == Id)
                    {
                        return true;
                    }
                }
                return false;
            }

            // Same rule the scheduler batches with (FSystemAccess::Conflicts), against the snapshot.
            bool Conflicts(const FSystemScheduleEntry& A, const FSystemScheduleEntry& B)
            {
                if (A.bExclusive || B.bExclusive)
                {
                    return true;
                }
                return FSystemAccess::Intersects(A.Writes, B.Writes)
                    || FSystemAccess::Intersects(A.Writes, B.Reads)
                    || FSystemAccess::Intersects(B.Writes, A.Reads);
            }

            // The access ids that actually force A and B apart.
            FString SharedAccessList(const FSystemScheduleEntry& A, const FSystemScheduleEntry& B)
            {
                if (A.bExclusive || B.bExclusive)
                {
                    return FString("exclusive (declares everything)");
                }

                TVector<uint32> Shared;
                for (uint32 Id : A.Writes)
                {
                    if ((Contains(B.Writes, Id) || Contains(B.Reads, Id)) && !Contains(Shared, Id))
                    {
                        Shared.push_back(Id);
                    }
                }
                for (uint32 Id : B.Writes)
                {
                    if (Contains(A.Reads, Id) && !Contains(Shared, Id))
                    {
                        Shared.push_back(Id);
                    }
                }
                return AccessList(Shared);
            }

            FString SystemLabel(const FSystemScheduleEntry& Entry, int32 Index)
            {
                if (!Entry.bManaged)
                {
                    return Entry.Name.ToString();
                }
                char Buffer[48] = {};
                snprintf(Buffer, sizeof(Buffer), LE_ICON_LANGUAGE_CSHARP " C# system %d", Index);
                return FString(Buffer);
            }

            // Measured with the font and size it will be drawn at, so boxes stay clean at any zoom.
            FString FitText(ImFont* Font, float FontSize, const char* Text, float MaxWidth)
            {
                if (Text == nullptr || Text[0] == '\0')
                {
                    return FString();
                }
                if (Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Text).x <= MaxWidth)
                {
                    return FString(Text);
                }

                const char* Ellipsis = "...";
                const float Budget = MaxWidth - Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Ellipsis).x;
                if (Budget <= 0.0f)
                {
                    return FString(Ellipsis);
                }

                FString Result(Text);
                while (!Result.empty() &&
                       Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Result.c_str()).x > Budget)
                {
                    Result.pop_back();
                }
                Result += Ellipsis;
                return Result;
            }

            ImVec4 CostColor(double Share)
            {
                if (Share > 0.25)
                {
                    return EditorColors::Danger();
                }
                if (Share > 0.10)
                {
                    return EditorColors::Warning();
                }
                return EditorColors::TextMuted();
            }

            void StripSeparator()
            {
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::TextColored(EditorColors::TextMuted(), "|");
                ImGui::SameLine(0.0f, 6.0f);
            }
        }
    }

    CWorld* FProfilerEditorTool::ResolveWorld() const
    {
        if (GWorldManager == nullptr)
        {
            return nullptr;
        }

        CWorld* Editor = nullptr;
        for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
        {
            if (!Context || !Context->World.IsValid())
            {
                continue;
            }
            // Prefer a live gameplay world (where every stage actually ticks); fall back to the editor world.
            if (Context->Type == EWorldType::Game || Context->Type == EWorldType::Simulation)
            {
                return Context->World.Get();
            }
            if (Context->Type == EWorldType::Editor)
            {
                Editor = Context->World.Get();
            }
        }
        return Editor;
    }

    void FProfilerEditorTool::RefreshSchedule()
    {
        if (CWorld* ResolvedWorld = ResolveWorld())
        {
            ResolvedWorld->GetSystemSchedule(Schedule);
        }
        else
        {
            Schedule.clear();
        }

        // The snapshot is emitted stage -> batch -> member, so columns are a run-length pass.
        ScheduleColumns.clear();
        for (int32 Index = 0; Index < (int32)Schedule.size(); ++Index)
        {
            const FSystemScheduleEntry& Entry = Schedule[Index];
            if (ScheduleColumns.empty() || ScheduleColumns.back().Stage != Entry.Stage || ScheduleColumns.back().Batch != Entry.Batch)
            {
                FScheduleColumn& Column = ScheduleColumns.emplace_back();
                Column.Stage = Entry.Stage;
                Column.Batch = Entry.Batch;
                Column.First = Index;
                Column.Count = 0;
            }
            ++ScheduleColumns.back().Count;
        }
    }

    int32 FProfilerEditorTool::ResolveSelection() const
    {
        const int32 Count = (int32)Schedule.size();
        if (SelectedIndex == INDEX_NONE)
        {
            return INDEX_NONE;
        }

        if (SelectedName.IsNone())
        {
            return SelectedIndex < Count ? SelectedIndex : INDEX_NONE;
        }

        if (SelectedIndex < Count && Schedule[SelectedIndex].Name == SelectedName)
        {
            return SelectedIndex;
        }
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (Schedule[Index].Name == SelectedName)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    const FGameplayProfileEntry* FProfilerEditorTool::FindStat(const char* Name) const
    {
        if (Name == nullptr || Name[0] == '\0')
        {
            return nullptr;
        }
        const auto Entry = Algo::FindIf(GameplayFrame.Entries,
            [Name](const FGameplayProfileEntry& Candidate) { return strcmp(Candidate.Name.c_str(), Name) == 0; });

        return Entry != GameplayFrame.Entries.end() ? &*Entry : nullptr;
    }

    void FProfilerEditorTool::DrawGameplay()
    {
        FGameplayProfiler& Prof = FGameplayProfiler::Get();

        if (!bFrozen)
        {
            GameplayFrame = Prof.GetLatest();
            RefreshSchedule();
        }

        ImGui::AlignTextToFramePadding();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Hold the last capture so it can be read without it moving under the cursor");
        }

        ImGui::SameLine(0.0f, 16.0f);
        if (bFrozen)
        {
            ImGui::TextColored(EditorColors::Warning(), LE_ICON_PAUSE " frozen");
        }
        else
        {
            const char Spinner[] = { '|', '/', '-', '\\' };
            ImGui::TextColored(EditorColors::Success(), "live %c", Spinner[(DrawTicks / 6) % 4]);
        }

        InsightsDetail::StripSeparator();
        ImGui::TextColored(EditorColors::TextDim(), "%d systems", (int32)Schedule.size());
        InsightsDetail::StripSeparator();
        ImGui::TextColored(EditorColors::TextDim(), "%d batches", (int32)ScheduleColumns.size());
        InsightsDetail::StripSeparator();

        constexpr double BudgetMs = 16.667;
        const double Share = GameplayFrame.TotalMs / BudgetMs;
        const ImVec4 HotColor = Share > 1.0 ? EditorColors::Danger()
            : (Share > 0.6 ? EditorColors::Warning() : EditorColors::Success());
        ImGui::TextColored(HotColor, "%.3f ms gameplay", GameplayFrame.TotalMs);

        if (ImGui::BeginTabBar("##insights"))
        {
            if (ImGui::BeginTabItem(LE_ICON_SITEMAP " Schedule"))
            {
                DrawSchedule();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(LE_ICON_TABLE " Stats"))
            {
                DrawStats();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(LE_ICON_INFORMATION " Detail"))
            {
                DrawDetail();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    // Schedule, the frame's parallel batches drawn as a dependency canvas.
    void FProfilerEditorTool::DrawSchedule()
    {
        if (Schedule.empty())
        {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped(ResolveWorld() == nullptr
                ? "No active world. Open a level, or enter Play, to inspect a system schedule."
                : "The active world has no registered systems.");
            ImGui::PopStyleColor();
            return;
        }

        ImGui::AlignTextToFramePadding();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("##ScheduleZoom", &ScheduleZoom, 0.6f, 1.6f, "zoom %.2fx");

        ImGui::SameLine(0.0f, 16.0f);
        ImGui::Checkbox("Conflict edges", &bShowEdges);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Link each system to the systems in the previous batch whose declared access it conflicts with: the reason it could not run any earlier");
        }

        const int32 Selection = ResolveSelection();
        if (Selection != INDEX_NONE)
        {
            ImGui::SameLine(0.0f, 16.0f);
            if (ImGui::SmallButton("Clear selection"))
            {
                SelectedIndex = INDEX_NONE;
                SelectedName  = FName();
            }
        }

        int32 WidestBatch = 0;
        int32 ExclusiveCount = 0;
        int32 ParallelCount = 0;
        for (const FSystemScheduleEntry& Entry : Schedule)
        {
            if (Entry.bExclusive)
            {
                ++ExclusiveCount;
            }
            else if (Entry.BatchSize > 1)
            {
                ++ParallelCount;
            }
        }
        for (const FScheduleColumn& Column : ScheduleColumns)
        {
            WidestBatch = Math::Max(WidestBatch, Column.Count);
        }

        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(EditorColors::TextPrimary(), "%d systems", (int32)Schedule.size());
        InsightsDetail::StripSeparator();
        ImGui::TextColored(EditorColors::Success(), "%d run in parallel", ParallelCount);
        if (ExclusiveCount > 0)
        {
            InsightsDetail::StripSeparator();
            ImGui::TextColored(EditorColors::Warning(), "%d exclusive", ExclusiveCount);
        }
        InsightsDetail::StripSeparator();
        ImGui::TextColored(EditorColors::TextDim(), "%d batches", (int32)ScheduleColumns.size());
        InsightsDetail::StripSeparator();
        ImGui::TextColored(EditorColors::TextDim(), "widest x%d", WidestBatch);

        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
        ImGui::TextWrapped("Columns are parallel batches: everything in one column runs concurrently on job-system workers, "
                           "and the next column waits for all of it. Stage bands are hard barriers. Click a system to see "
                           "every system in its stage it can never share a batch with.");
        ImGui::PopStyleColor();
        ImGui::Separator();

        DrawScheduleCanvas();
    }

    void FProfilerEditorTool::DrawScheduleCanvas()
    {
        const float Scale = ScheduleZoom;

        // The editor's DPI scale changes font size independently of zoom, so a fixed height clips.
        ImFont*     Font      = ImGui::GetFont();
        const float FontSize  = ImGui::GetFontSize() * Scale;
        const float SmallFont = FontSize * 0.86f;

        const float InnerPad = 10.0f * Scale;
        const float PadY     = 8.0f * Scale;
        const float LineGap  = 5.0f * Scale;

        const float NodeH        = PadY * 2.0f + FontSize + LineGap + SmallFont + LineGap + SmallFont;
        const float NodeW        = Math::Max(250.0f * Scale, FontSize * 15.0f);
        const float ColGap       = 54.0f * Scale;
        const float RowGap       = 16.0f * Scale;
        const float BatchHeaderH = SmallFont + 8.0f * Scale;
        const float StageHeaderH = SmallFont + 10.0f * Scale;
        const float HeaderH      = StageHeaderH + BatchHeaderH;
        const float Pad          = 14.0f * Scale;

        const int32 NumColumns = (int32)ScheduleColumns.size();
        const int32 NumEntries = (int32)Schedule.size();

        float MaxColumnHeight = 0.0f;
        for (const FScheduleColumn& Column : ScheduleColumns)
        {
            const float Height = (float)Column.Count * NodeH + (float)(Column.Count - 1) * RowGap;
            MaxColumnHeight = Math::Max(MaxColumnHeight, Height);
        }

        // Column-local positions (canvas space); each column is vertically centered.
        TVector<ImVec2> Positions;
        TVector<int32>  ColumnOf;
        Positions.resize(NumEntries, ImVec2(0.0f, 0.0f));
        ColumnOf.resize(NumEntries, 0);
        for (int32 L = 0; L < NumColumns; ++L)
        {
            const FScheduleColumn& Column = ScheduleColumns[L];
            const float Height = (float)Column.Count * NodeH + (float)(Column.Count - 1) * RowGap;
            float Y = (MaxColumnHeight - Height) * 0.5f;
            for (int32 Row = 0; Row < Column.Count; ++Row)
            {
                const int32 Index = Column.First + Row;
                Positions[Index] = ImVec2((float)L * (NodeW + ColGap), Y);
                ColumnOf[Index]  = L;
                Y += NodeH + RowGap;
            }
        }

        const float TotalW = (float)NumColumns * NodeW + (float)(NumColumns - 1) * ColGap;
        const float TotalH = MaxColumnHeight;

        ImGui::BeginChild("##ScheduleCanvas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

        const ImVec2 CanvasCursor = ImGui::GetCursorScreenPos();
        const ImVec2 Origin(CanvasCursor.x + Pad + ColGap * 0.35f, CanvasCursor.y + Pad + HeaderH);
        ImDrawList* DL = ImGui::GetWindowDrawList();
        const bool bWindowHovered = ImGui::IsWindowHovered();

        const int32 Selection = ResolveSelection();

        // Stage bands span every batch of a stage, showing where the hard barriers are.
        for (int32 L = 0; L < NumColumns; )
        {
            const uint8 Stage = ScheduleColumns[L].Stage;
            int32 Last = L;
            while (Last + 1 < NumColumns && ScheduleColumns[Last + 1].Stage == Stage)
            {
                ++Last;
            }

            const ImVec4 StageTint = InsightsDetail::StageColor(Stage);
            const ImVec2 BandMin(Origin.x + (float)L * (NodeW + ColGap) - ColGap * 0.35f, Origin.y - HeaderH);
            const ImVec2 BandMax(Origin.x + (float)Last * (NodeW + ColGap) + NodeW + ColGap * 0.35f, Origin.y + TotalH + Pad * 0.75f);

            DL->AddRectFilled(BandMin, BandMax, EditorColors::U32(EditorColors::WithAlpha(StageTint, 0.09f)), 8.0f * Scale);
            DL->AddRect(BandMin, BandMax, EditorColors::U32(EditorColors::WithAlpha(StageTint, 0.32f)), 8.0f * Scale, 0, 1.0f * Scale);

            int32 StageSystems = 0;
            for (int32 C = L; C <= Last; ++C)
            {
                StageSystems += ScheduleColumns[C].Count;
            }

            const int32 StageBatches = Last - L + 1;
            char Header[128];
            snprintf(Header, sizeof(Header), "%s   %d system%s  /  %d batch%s",
                     InsightsDetail::StageName(Stage), StageSystems, StageSystems == 1 ? "" : "s",
                     StageBatches, StageBatches == 1 ? "" : "es");
            DL->AddText(Font, SmallFont, ImVec2(BandMin.x + 10.0f * Scale, BandMin.y + 4.0f * Scale),
                        EditorColors::U32(StageTint), Header);

            L = Last + 1;
        }

        // Per-batch bands inside the stage band.
        for (int32 L = 0; L < NumColumns; ++L)
        {
            const FScheduleColumn& Column = ScheduleColumns[L];
            const float BandX = Origin.x + (float)L * (NodeW + ColGap);
            const ImVec2 BandMin(BandX - RowGap * 0.5f, Origin.y - BatchHeaderH);
            const ImVec2 BandMax(BandX + NodeW + RowGap * 0.5f, Origin.y + TotalH + Pad * 0.35f);

            DL->AddRectFilled(BandMin, BandMax,
                              EditorColors::U32(EditorColors::WithAlpha(EditorColors::PanelBg(), (L % 2) ? 0.55f : 0.28f)),
                              6.0f * Scale);

            char Header[64];
            if (Column.Count > 1)
            {
                snprintf(Header, sizeof(Header), "Batch %d   x%d parallel", (int32)Column.Batch, Column.Count);
            }
            else
            {
                snprintf(Header, sizeof(Header), "Batch %d   serial", (int32)Column.Batch);
            }
            DL->AddText(Font, SmallFont, ImVec2(BandMin.x + 8.0f * Scale, Origin.y - BatchHeaderH + 2.0f * Scale),
                        EditorColors::U32(Column.Count > 1 ? EditorColors::Success() : EditorColors::TextMuted()), Header);
        }

        // Only within a stage, since a stage barrier already serializes everything across one.
        if (bShowEdges)
        {
            for (int32 Index = 0; Index < NumEntries; ++Index)
            {
                const int32 L = ColumnOf[Index];
                if (L == 0 || ScheduleColumns[L].Stage != ScheduleColumns[L - 1].Stage)
                {
                    continue;
                }

                const FScheduleColumn& Prev = ScheduleColumns[L - 1];
                for (int32 Row = 0; Row < Prev.Count; ++Row)
                {
                    const int32 Other = Prev.First + Row;
                    if (!InsightsDetail::Conflicts(Schedule[Index], Schedule[Other]))
                    {
                        continue;
                    }

                    const ImVec2 From(Origin.x + Positions[Other].x + NodeW, Origin.y + Positions[Other].y + NodeH * 0.5f);
                    const ImVec2 To(Origin.x + Positions[Index].x, Origin.y + Positions[Index].y + NodeH * 0.5f);
                    const float Curve = (To.x - From.x) * 0.5f;

                    const bool bTouchesSelection = Selection != INDEX_NONE && (Index == Selection || Other == Selection);
                    const bool bDimmed = Selection != INDEX_NONE && !bTouchesSelection;

                    ImVec4 LinkColor = bTouchesSelection ? EditorColors::Danger() : EditorColors::TextDim();
                    LinkColor = EditorColors::WithAlpha(LinkColor, bDimmed ? 0.10f : (bTouchesSelection ? 0.95f : 0.38f));

                    DL->AddBezierCubic(From, ImVec2(From.x + Curve, From.y), ImVec2(To.x - Curve, To.y), To,
                                       EditorColors::U32(LinkColor), (bTouchesSelection ? 2.6f : 1.6f) * Scale);
                }
            }
        }

        int32 HoveredIndex = INDEX_NONE;

        for (int32 Index = 0; Index < NumEntries; ++Index)
        {
            const FSystemScheduleEntry& Entry = Schedule[Index];

            const bool bIsSelected = Index == Selection;
            const bool bConflicts  = Selection != INDEX_NONE && !bIsSelected && InsightsDetail::Conflicts(Entry, Schedule[Selection]);
            const float Alpha      = (Selection == INDEX_NONE || bIsSelected || bConflicts) ? 1.0f : 0.26f;

            const ImVec2 Min(Origin.x + Positions[Index].x, Origin.y + Positions[Index].y);
            const ImVec2 Max(Min.x + NodeW, Min.y + NodeH);
            const float  Rounding = 6.0f * Scale;

            const ImVec4 StageTint = InsightsDetail::StageColor(Entry.Stage);

            ImVec4 BorderColor = StageTint;
            float  BorderWidth = 1.4f;
            if (bIsSelected)
            {
                BorderColor = EditorColors::Accent();
                BorderWidth = 2.6f;
            }
            else if (bConflicts)
            {
                BorderColor = EditorColors::Danger();
                BorderWidth = 2.0f;
            }
            else if (Entry.bExclusive)
            {
                BorderColor = EditorColors::Warning();
            }

            DL->AddRectFilled(Min, Max, EditorColors::U32(EditorColors::WithAlpha(EditorColors::FrameBg(), Alpha)), Rounding);
            DL->AddRect(Min, Max, EditorColors::U32(EditorColors::WithAlpha(BorderColor, Alpha)), Rounding, 0, BorderWidth * Scale);

            // Clip is a safety net only; every string below is measured and ellipsized to fit.
            DL->PushClipRect(Min, Max, true);

            const float TextX     = Min.x + InnerPad;
            const float DotOffset = 13.0f * Scale;
            const float BodyMaxW  = NodeW - InnerPad * 2.0f;
            float TextY = Min.y + PadY;

            const FString Label = InsightsDetail::SystemLabel(Entry, Index);
            const FGameplayProfileEntry* Stat = Entry.bManaged ? nullptr : FindStat(Label.c_str());

            // Measured first so the title can reserve room instead of running underneath it.
            char Badge[40] = {};
            ImVec4 BadgeColor = EditorColors::TextMuted();
            if (Stat != nullptr)
            {
                snprintf(Badge, sizeof(Badge), "%.3f ms", Stat->InclusiveMs);
                BadgeColor = InsightsDetail::CostColor(GameplayFrame.TotalMs > 0.0 ? Stat->InclusiveMs / GameplayFrame.TotalMs : 0.0);
            }
            else if (Entry.bExclusive)
            {
                snprintf(Badge, sizeof(Badge), "%s", LE_ICON_LOCK " alone");
                BadgeColor = EditorColors::Warning();
            }
            const ImVec2 BadgeSize = Badge[0] != '\0'
                ? Font->CalcTextSizeA(SmallFont, FLT_MAX, 0.0f, Badge)
                : ImVec2(0.0f, 0.0f);

            DL->AddCircleFilled(ImVec2(TextX + 3.0f * Scale, TextY + FontSize * 0.5f), 3.5f * Scale,
                                EditorColors::U32(EditorColors::WithAlpha(StageTint, Alpha)));

            const float TitleMaxW = BodyMaxW - DotOffset - BadgeSize.x - 6.0f * Scale;
            const FString Title = InsightsDetail::FitText(Font, FontSize, Label.c_str(), TitleMaxW);
            DL->AddText(Font, FontSize, ImVec2(TextX + DotOffset, TextY),
                        EditorColors::U32(EditorColors::WithAlpha(EditorColors::TextPrimary(), Alpha)), Title.c_str());

            if (Badge[0] != '\0')
            {
                DL->AddText(Font, SmallFont, ImVec2(Max.x - InnerPad - BadgeSize.x, TextY + 1.0f * Scale),
                            EditorColors::U32(EditorColors::WithAlpha(BadgeColor, Alpha)), Badge);
            }

            TextY += FontSize + LineGap;

            if (Entry.bExclusive)
            {
                const FString ExclusiveLine = InsightsDetail::FitText(Font, SmallFont, "exclusive: conflicts with everything", BodyMaxW);
                DL->AddText(Font, SmallFont, ImVec2(TextX, TextY),
                            EditorColors::U32(EditorColors::WithAlpha(EditorColors::Warning(), Alpha)), ExclusiveLine.c_str());
            }
            else
            {
                const FString Writes = InsightsDetail::AccessList(Entry.Writes);
                FString WriteLine = "W  ";
                WriteLine += Writes.empty() ? "-" : Writes.c_str();
                const FString Fitted = InsightsDetail::FitText(Font, SmallFont, WriteLine.c_str(), BodyMaxW);
                DL->AddText(Font, SmallFont, ImVec2(TextX, TextY),
                            EditorColors::U32(EditorColors::WithAlpha(
                                Writes.empty() ? EditorColors::TextMuted() : EditorColors::Danger(), Alpha)), Fitted.c_str());

                TextY += SmallFont + LineGap;

                const FString Reads = InsightsDetail::AccessList(Entry.Reads);
                FString ReadLine = "R  ";
                ReadLine += Reads.empty() ? "-" : Reads.c_str();
                const FString FittedReads = InsightsDetail::FitText(Font, SmallFont, ReadLine.c_str(), BodyMaxW);
                DL->AddText(Font, SmallFont, ImVec2(TextX, TextY),
                            EditorColors::U32(EditorColors::WithAlpha(
                                Reads.empty() ? EditorColors::TextMuted() : EditorColors::Accent(), Alpha)), FittedReads.c_str());
            }

            DL->PopClipRect();

            if (bWindowHovered && ImGui::IsMouseHoveringRect(Min, Max))
            {
                HoveredIndex = Index;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    SelectedIndex = Index;
                    SelectedName  = Entry.Name;
                }

                ImGui::BeginTooltip();
                ImGui::TextColored(StageTint, "%s", Label.c_str());
                ImGui::Separator();
                ImGui::Text("Stage       %s", InsightsDetail::StageName(Entry.Stage));
                ImGui::Text("Batch       %d   (%d system%s)", (int32)Entry.Batch, (int32)Entry.BatchSize,
                            Entry.BatchSize == 1 ? "" : "s");
                ImGui::Text("Priority    %d", (int32)Entry.Priority);
                ImGui::Separator();
                if (Entry.bExclusive)
                {
                    ImGui::TextColored(EditorColors::Warning(), "Exclusive: declares everything, so it runs alone.");
                    ImGui::TextColored(EditorColors::TextMuted(), "A system with no Access member defaults to this.");
                }
                else
                {
                    const FString Writes = InsightsDetail::AccessList(Entry.Writes);
                    const FString Reads  = InsightsDetail::AccessList(Entry.Reads);
                    ImGui::TextColored(EditorColors::Danger(), "Writes      %s", Writes.empty() ? "(none)" : Writes.c_str());
                    ImGui::TextColored(EditorColors::Accent(), "Reads       %s", Reads.empty() ? "(none)" : Reads.c_str());
                }
                if (Stat != nullptr)
                {
                    ImGui::Separator();
                    ImGui::TextColored(EditorColors::TextDim(), "%.3f ms over %d call%s", Stat->InclusiveMs,
                                       (int32)Stat->Calls, Stat->Calls == 1 ? "" : "s");
                }
                if (Entry.bManaged)
                {
                    ImGui::Separator();
                    ImGui::TextColored(EditorColors::TextMuted(), "C# systems schedule without a native name; find this one\nby its type name in the Stats tab.");
                }
                ImGui::EndTooltip();
            }
        }

        if (bWindowHovered && HoveredIndex == INDEX_NONE && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            SelectedIndex = INDEX_NONE;
            SelectedName  = FName();
        }

        ImGui::Dummy(ImVec2(TotalW + Pad * 2.0f + ColGap, TotalH + HeaderH + Pad * 2.0f));
        ImGui::EndChild();
    }

    // Stats, aggregate per-scope CPU timings for scripts, C# systems and sample scopes.
    void FProfilerEditorTool::DrawStats()
    {
        FGameplayProfiler& Prof = FGameplayProfiler::Get();

        const double TotalMs = GameplayFrame.TotalMs;
        constexpr double BudgetMs = 16.667;
        const double Share = TotalMs / BudgetMs;
        const ImVec4 HotColor = Share > 1.0 ? EditorColors::Danger()
            : (Share > 0.6 ? EditorColors::Warning() : EditorColors::Success());

        uint32 TotalCalls = 0;
        for (const FGameplayProfileEntry& Entry : GameplayFrame.Entries)
        {
            TotalCalls += Entry.Calls;
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(EditorColors::TextPrimary(), "%.3f ms", TotalMs);
        InsightsDetail::StripSeparator();
        ImGui::TextColored(HotColor, "%.0f%% of 16.7 ms", Share * 100.0);
        InsightsDetail::StripSeparator();
        ImGui::TextColored(EditorColors::TextDim(), "%d scopes", (int32)GameplayFrame.Entries.size());
        InsightsDetail::StripSeparator();
        ImGui::TextColored(EditorColors::TextDim(), "%d calls", (int32)TotalCalls);

        const FProfilerHistory& History = Prof.GetFrameTotalHistory();
        if (!History.Values.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_PlotLines, EditorColors::Accent());
            ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorColors::WithAlpha(EditorColors::PanelBg(), 0.55f));
            ImGui::PlotLines("##frametotals", History.Values.data(), (int)History.Values.size(), (int)History.Offset, nullptr, 0.0f, FLT_MAX, ImVec2(-1.0f, 44.0f));
            ImGui::PopStyleColor(2);
        }

        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputTextWithHint("##filter", LE_ICON_MAGNIFY " Filter scopes", Filter, sizeof(Filter));

        const ImGuiTableFlags Flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
            | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;

        if (ImGui::BeginTable("##gpstats", 7, Flags))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Scope",    ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Calls",    ImGuiTableColumnFlags_WidthFixed,  52.0f);
            ImGui::TableSetupColumn("Total ms", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_DefaultSort, 72.0f);
            ImGui::TableSetupColumn("Self ms",  ImGuiTableColumnFlags_WidthFixed,  72.0f);
            ImGui::TableSetupColumn("Avg ms",   ImGuiTableColumnFlags_WidthFixed,  72.0f);
            ImGui::TableSetupColumn("Share",    ImGuiTableColumnFlags_WidthFixed,  72.0f);
            ImGui::TableSetupColumn("History",  ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 1.4f);
            ImGui::TableHeadersRow();

            TVector<const FGameplayProfileEntry*> Rows;
            Rows.reserve(GameplayFrame.Entries.size());
            for (const FGameplayProfileEntry& Entry : GameplayFrame.Entries)
            {
                if (Filter[0] == '\0' || Entry.Name.find(Filter) != FFixedString::npos)
                {
                    Rows.push_back(&Entry);
                }
            }

            int  SortCol = 2;
            bool Ascending = false;
            if (ImGuiTableSortSpecs* Specs = ImGui::TableGetSortSpecs())
            {
                if (Specs->SpecsCount > 0)
                {
                    SortCol   = Specs->Specs[0].ColumnIndex;
                    Ascending = Specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                }
            }

            auto Avg = [](const FGameplayProfileEntry* E) { return E->Calls ? E->InclusiveMs / E->Calls : 0.0; };
            Algo::Sort(Rows, [&](const FGameplayProfileEntry* A, const FGameplayProfileEntry* B)
            {
                double Cmp;
                switch (SortCol)
                {
                    case 0:  Cmp = static_cast<double>(A->Name.compare(B->Name)); break;
                    case 1:  Cmp = static_cast<double>(A->Calls) - static_cast<double>(B->Calls); break;
                    case 3:  Cmp = A->ExclusiveMs - B->ExclusiveMs; break;
                    case 4:  Cmp = Avg(A) - Avg(B); break;
                    default: Cmp = A->InclusiveMs - B->InclusiveMs; break;
                }
                return Ascending ? (Cmp < 0.0) : (Cmp > 0.0);
            });

            for (const FGameplayProfileEntry* Entry : Rows)
            {
                const double RowShare = (TotalMs > 0.0) ? (Entry->InclusiveMs / TotalMs) : 0.0;
                const ImVec4 RowColor = InsightsDetail::CostColor(RowShare);

                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Entry->Name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", (int32)Entry->Calls);
                ImGui::TableNextColumn(); ImGui::TextColored(RowColor, "%.3f", Entry->InclusiveMs);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", Entry->ExclusiveMs);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", Avg(Entry));

                // Share reads as a bar first and a number second, so an outlier is findable without reading.
                ImGui::TableNextColumn();
                const ImVec2 BarMin = ImGui::GetCursorScreenPos();
                const float  BarW   = ImGui::GetContentRegionAvail().x;
                const float  BarH   = ImGui::GetTextLineHeight();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    BarMin,
                    ImVec2(BarMin.x + BarW * Math::Clamp((float)RowShare, 0.0f, 1.0f), BarMin.y + BarH),
                    EditorColors::U32(EditorColors::WithAlpha(RowColor, 0.30f)), 2.0f);
                ImGui::Text("%.0f%%", RowShare * 100.0);

                ImGui::TableNextColumn();
                if (const FProfilerHistory* EntryHistory = Prof.GetEntryHistory(Entry->Hash))
                {
                    if (!EntryHistory->Values.empty())
                    {
                        ImGui::PushID(Entry);
                        ImGui::PushStyleColor(ImGuiCol_PlotLines, EditorColors::WithAlpha(RowColor, 0.9f));
                        ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorColors::WithAlpha(EditorColors::PanelBg(), 0.45f));
                        ImGui::PlotLines("##h", EntryHistory->Values.data(), static_cast<int>(EntryHistory->Values.size()), (int)EntryHistory->Offset, nullptr, 0.0f, FLT_MAX, ImVec2(-1.0f, 18.0f));
                        ImGui::PopStyleColor(2);
                        ImGui::PopID();
                    }
                }
            }

            ImGui::EndTable();
        }

        if (GameplayFrame.Entries.empty())
        {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped("No gameplay scopes yet. Enter Play to capture script and system updates, or instrument "
                               "C# with  using (Profiler.Sample(\"Name\")) { ... }");
            ImGui::PopStyleColor();
        }
    }

    // Detail, the full information for the system selected on the Schedule canvas.
    void FProfilerEditorTool::DrawDetail()
    {
        const int32 Selection = ResolveSelection();
        if (Selection == INDEX_NONE)
        {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped("Select a system on the Schedule canvas to inspect it here.");
            ImGui::PopStyleColor();
            return;
        }

        const FSystemScheduleEntry& Entry = Schedule[Selection];
        const FString Label     = InsightsDetail::SystemLabel(Entry, Selection);
        const ImVec4  StageTint = InsightsDetail::StageColor(Entry.Stage);

        ImGui::PushStyleColor(ImGuiCol_Text, StageTint);
        ImGui::SeparatorText(Label.c_str());
        ImGui::PopStyleColor();

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(StageTint, "%s", InsightsDetail::StageName(Entry.Stage));
        InsightsDetail::StripSeparator();
        ImGui::TextColored(EditorColors::TextDim(), "batch %d", (int32)Entry.Batch);
        InsightsDetail::StripSeparator();
        ImGui::TextColored(EditorColors::TextDim(), "priority %d", (int32)Entry.Priority);
        InsightsDetail::StripSeparator();
        if (Entry.bExclusive)
        {
            ImGui::TextColored(EditorColors::Warning(), LE_ICON_LOCK " runs alone");
        }
        else if (Entry.BatchSize > 1)
        {
            ImGui::TextColored(EditorColors::Success(), LE_ICON_LIGHTNING_BOLT " parallel with %d other%s",
                               (int32)Entry.BatchSize - 1, Entry.BatchSize == 2 ? "" : "s");
        }
        else
        {
            ImGui::TextColored(EditorColors::TextMuted(), "parallel-capable, alone this frame");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Declared access");
        if (Entry.bExclusive)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped("This system declares no FSystemAccess, so the scheduler treats it as writing everything. "
                               "Declaring reads and writes lets it batch with other systems.");
            ImGui::PopStyleColor();
        }
        else
        {
            const FString Writes = InsightsDetail::AccessList(Entry.Writes);
            const FString Reads  = InsightsDetail::AccessList(Entry.Reads);
            ImGui::TextColored(EditorColors::Danger(), "Writes");
            ImGui::TextUnformatted(Writes.empty() ? "(none)" : Writes.c_str());
            ImGui::Spacing();
            ImGui::TextColored(EditorColors::Accent(), "Reads");
            ImGui::TextUnformatted(Reads.empty() ? "(none)" : Reads.c_str());
        }

        // Cross-stage pairs are omitted, since a stage barrier already serializes those.
        TVector<int32> ConflictIndices;
        for (int32 Index = 0; Index < (int32)Schedule.size(); ++Index)
        {
            if (Index != Selection && Schedule[Index].Stage == Entry.Stage && InsightsDetail::Conflicts(Entry, Schedule[Index]))
            {
                ConflictIndices.push_back(Index);
            }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Serialization");

        if (ConflictIndices.empty())
        {
            ImGui::TextColored(EditorColors::Success(), "Nothing in %s conflicts with this system.", InsightsDetail::StageName(Entry.Stage));
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped("It can run in the stage's first batch alongside everything else.");
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped("These %d system%s can never share a batch with this one. Each row is a serialization point.",
                               (int32)ConflictIndices.size(), ConflictIndices.size() == 1 ? "" : "s");
            ImGui::PopStyleColor();

            const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;

            if (ImGui::BeginTable("##conflicts", 3, Flags))
            {
                ImGui::TableSetupColumn("System",        ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableSetupColumn("Batch",         ImGuiTableColumnFlags_WidthFixed, 88.0f);
                ImGui::TableSetupColumn("Shared access", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableHeadersRow();

                for (int32 Index : ConflictIndices)
                {
                    const FSystemScheduleEntry& Other = Schedule[Index];

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(Index);
                    if (ImGui::Selectable(InsightsDetail::SystemLabel(Other, Index).c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                    {
                        SelectedIndex = Index;
                        SelectedName  = Other.Name;
                    }
                    ImGui::PopID();

                    ImGui::TableNextColumn();
                    if (Other.Batch == Entry.Batch)
                    {
                        ImGui::TextColored(EditorColors::Danger(), "%d (!)", (int32)Other.Batch);
                    }
                    else
                    {
                        ImGui::TextColored(EditorColors::TextDim(), "%d %s", (int32)Other.Batch,
                                           Other.Batch < Entry.Batch ? "before" : "after");
                    }

                    ImGui::TableNextColumn();
                    const FString SharedList = InsightsDetail::SharedAccessList(Entry, Other);
                    ImGui::TextColored(EditorColors::Warning(), "%s", SharedList.empty() ? "(none)" : SharedList.c_str());
                }

                ImGui::EndTable();
            }
        }

        if (const FGameplayProfileEntry* Stat = Entry.bManaged ? nullptr : FindStat(Label.c_str()))
        {
            ImGui::Spacing();
            ImGui::SeparatorText("Timing (last frame)");
            ImGui::TextColored(EditorColors::TextDim(), "%d call%s", (int32)Stat->Calls, Stat->Calls == 1 ? "" : "s");
            InsightsDetail::StripSeparator();
            ImGui::TextColored(InsightsDetail::CostColor(GameplayFrame.TotalMs > 0.0 ? Stat->InclusiveMs / GameplayFrame.TotalMs : 0.0),
                               "%.3f ms inclusive", Stat->InclusiveMs);
            InsightsDetail::StripSeparator();
            ImGui::TextColored(EditorColors::TextDim(), "%.3f ms self", Stat->ExclusiveMs);
        }
    }
}
