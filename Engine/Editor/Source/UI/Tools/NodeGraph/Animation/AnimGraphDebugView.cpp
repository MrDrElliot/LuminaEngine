#include "AnimGraphDebugView.h"

#include "Animation/TaskSystem/AnimTask.h"
#include "Assets/AssetTypes/Animation/BlendSpace/BlendSpace.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Containers/StringFormat.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "imgui_internal.h"

namespace Lumina
{
    static constexpr ImU32 kPlotBackground = IM_COL32(18, 19, 22, 255);
    static constexpr ImU32 kPlotGrid       = IM_COL32(52, 55, 62, 255);
    static constexpr ImU32 kPlotLabel      = IM_COL32(150, 155, 165, 255);
    static constexpr ImU32 kPlayhead       = IM_COL32(240, 240, 245, 255);
    static constexpr float kPlotRounding   = 3.0f;
    static constexpr int32 kCurveSamples   = 48;
    static constexpr int32 kMaxClipCurves  = 4;

    TOptional<float> FAnimGraphDebugView::ReadPin(const CEdNodeGraphPin* Pin) const
    {
        if (!bLive || Pin == nullptr)
        {
            return {};
        }

        auto It = PinRegisters.find(Pin);
        if (It == PinRegisters.end() && Pin->HasConnection())
        {
            It = PinRegisters.find(Pin->GetConnection(0));
        }

        if (It == PinRegisters.end() || It->second >= Scalars.size())
        {
            return {};
        }
        return Scalars[It->second];
    }

    TOptional<float> FAnimGraphDebugView::ReadPlayhead(const CEdGraphNode* Node) const
    {
        auto It = Playheads.find(Node);
        if (!bLive || It == Playheads.end() || It->second >= Scalars.size())
        {
            return {};
        }
        return Scalars[It->second];
    }

    const FAnimValueHistory* FAnimGraphDebugView::FindHistory(const CEdNodeGraphPin* Pin) const
    {
        auto It = Histories.find(Pin);
        return It != Histories.end() ? &It->second : nullptr;
    }

    float FAnimGraphDebugView::GetNodeWeight(const CEdGraphNode* Node) const
    {
        auto It = NodeWeights.find(Node);
        return It != NodeWeights.end() ? It->second : 1.0f;
    }

    namespace AnimDebugDraw
    {
        namespace
        {
            float SmallFontSize() { return ImGui::GetFontSize() * 0.85f; }

            void SmallText(ImDrawList* DrawList, ImVec2 Position, ImU32 Color, const char* Text)
            {
                DrawList->AddText(ImGui::GetFont(), SmallFontSize(), Position, Color, Text);
            }

            ImVec2 SmallTextSize(const char* Text)
            {
                return ImGui::GetFont()->CalcTextSizeA(SmallFontSize(), FLT_MAX, 0.0f, Text);
            }

            ImU32 ToColor(const FVector4& Color, float Alpha = 1.0f)
            {
                return ImGui::ColorConvertFloat4ToU32(ImVec4(Color.x, Color.y, Color.z, Color.w * Alpha));
            }

            // Reserves Size at the cursor as one item and returns its top-left corner.
            ImVec2 ReserveBox(ImVec2 Size)
            {
                const ImVec2 Min = ImGui::GetCursorScreenPos();
                ImGui::Dummy(Size);
                return Min;
            }

            void CenteredHint(ImDrawList* DrawList, ImVec2 Min, ImVec2 Size, const char* Text)
            {
                const ImVec2 TextSize = SmallTextSize(Text);
                SmallText(DrawList, ImVec2(Min.x + (Size.x - TextSize.x) * 0.5f, Min.y + (Size.y - TextSize.y) * 0.5f), kPlotLabel, Text);
            }
        }

        void Plot(const FAnimValueHistory& Values, ImVec2 Size, ImU32 Color)
        {
            const ImVec2 Min = ReserveBox(Size);
            const ImVec2 Max(Min.x + Size.x, Min.y + Size.y);
            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            DrawList->AddRectFilled(Min, Max, kPlotBackground, kPlotRounding);

            if (Values.size() < 2)
            {
                CenteredHint(DrawList, Min, Size, "No live values");
                return;
            }

            float Low = Values[0];
            float High = Values[0];
            for (size_t Index = 1; Index < Values.size(); ++Index)
            {
                Low = Math::Min(Low, Values[Index]);
                High = Math::Max(High, Values[Index]);
            }

            // A flat trace sits in the middle instead of on an edge.
            const float Span = High - Low > 1e-4f ? High - Low : 1.0f;
            const float Center = (High + Low) * 0.5f;
            const float Bottom = High - Low > 1e-4f ? Low : Center - 0.5f;
            const float Inset = 4.0f;
            const float Step = (Size.x - Inset * 2.0f) / (float)(kAnimValueHistoryLength - 1);

            TVector<ImVec2> Points;
            Points.reserve(Values.size());
            for (size_t Index = 0; Index < Values.size(); ++Index)
            {
                const float X = Max.x - Inset - (float)(Values.size() - 1 - Index) * Step;
                const float T = (Values[Index] - Bottom) / Span;
                Points.emplace_back(X, Max.y - Inset - T * (Size.y - Inset * 2.0f));
            }
            DrawList->AddPolyline(Points.data(), (int)Points.size(), Color, 1.5f, ImDrawFlags_None);
            DrawList->AddCircleFilled(Points.back(), 2.5f, Color);

            const FFixedString Current = FormatAs<FFixedString>("{:.3f}", Values.back());
            SmallText(DrawList, ImVec2(Max.x - SmallTextSize(Current.c_str()).x - Inset, Min.y + 2.0f), Color, Current.c_str());
            if (High - Low > 1e-4f)
            {
                SmallText(DrawList, ImVec2(Min.x + Inset, Min.y + 2.0f), kPlotLabel, FormatAs<FFixedString>("{:.2f}", High).c_str());
                SmallText(DrawList, ImVec2(Min.x + Inset, Max.y - SmallFontSize() - 2.0f), kPlotLabel, FormatAs<FFixedString>("{:.2f}", Low).c_str());
            }
        }

        void ClipTimeline(const CAnimation* Clip, TOptional<float> Time, float Width)
        {
            const float Duration = Clip != nullptr ? Clip->GetDuration() : 0.0f;
            const int32 CurveCount = Clip != nullptr ? Math::Min((int32)Clip->GetCurves().size(), kMaxClipCurves) : 0;

            const float LineHeight  = SmallFontSize() + 4.0f;
            const float TrackHeight = 10.0f;
            const float CurveHeight = CurveCount > 0 ? 34.0f : 0.0f;
            const float LegendHeight = CurveCount > 0 ? LineHeight : 0.0f;
            const ImVec2 Size(Width, LineHeight + TrackHeight + 4.0f + CurveHeight + LegendHeight);
            const ImVec2 Min = ReserveBox(Size);
            ImDrawList* DrawList = ImGui::GetWindowDrawList();

            const FFixedString TimeText = Time.has_value()
                ? FormatAs<FFixedString>("{:.2f} / {:.2f} s", Time.value(), Duration)
                : FormatAs<FFixedString>("{:.2f} s", Duration);
            SmallText(DrawList, Min, kPlotLabel, Clip != nullptr ? Clip->GetName().c_str() : "Clip from the Animation pin");
            SmallText(DrawList, ImVec2(Min.x + Width - SmallTextSize(TimeText.c_str()).x, Min.y), kPlotLabel, TimeText.c_str());

            const ImVec2 TrackMin(Min.x, Min.y + LineHeight);
            const ImVec2 TrackMax(Min.x + Width, TrackMin.y + TrackHeight);
            DrawList->AddRectFilled(TrackMin, TrackMax, kPlotBackground, kPlotRounding);
            if (Duration <= 1e-5f)
            {
                return;
            }

            const auto TimeToX = [&](float At) { return TrackMin.x + Width * Math::Clamp(At / Duration, 0.0f, 1.0f); };
            const TOptional<float> Playhead = Time.has_value() ? TOptional<float>(std::fmod(Math::Max(Time.value(), 0.0f), Duration + 1e-4f)) : TOptional<float>();
            if (Playhead.has_value())
            {
                DrawList->AddRectFilled(TrackMin, ImVec2(TimeToX(Playhead.value()), TrackMax.y), EditorColors::ScaleAlpha(EditorColors::U32(EditorColors::Accent()), 0.45f), kPlotRounding);
            }

            for (const FAnimationNotifyState& State : Clip->GetNotifyStates())
            {
                DrawList->AddRectFilled(ImVec2(TimeToX(State.StartTime), TrackMax.y - 3.0f), ImVec2(TimeToX(State.EndTime), TrackMax.y), ToColor(State.Color, 0.85f));
            }
            for (const FAnimationNotify& Notify : Clip->GetNotifies())
            {
                const float X = TimeToX(Notify.Time);
                DrawList->AddLine(ImVec2(X, TrackMin.y), ImVec2(X, TrackMax.y), ToColor(Notify.Color), 2.0f);
            }

            const ImVec2 CurveMin(Min.x, TrackMax.y + 4.0f);
            if (CurveCount > 0)
            {
                DrawList->AddRectFilled(CurveMin, ImVec2(Min.x + Width, CurveMin.y + CurveHeight), kPlotBackground, kPlotRounding);
                float LegendX = Min.x;
                for (int32 CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
                {
                    const FAnimationCurve& Curve = Clip->GetCurves()[CurveIndex];
                    float Low = 0.0f;
                    float High = 1.0f;
                    Curve.Curve.GetValueRange(Low, High);
                    const float Span = High - Low > 1e-4f ? High - Low : 1.0f;

                    ImVec2 Points[kCurveSamples];
                    for (int32 Sample = 0; Sample < kCurveSamples; ++Sample)
                    {
                        const float At = Duration * (float)Sample / (float)(kCurveSamples - 1);
                        const float T = (Curve.Curve.Evaluate(At) - Low) / Span;
                        Points[Sample] = ImVec2(TimeToX(At), CurveMin.y + CurveHeight - 3.0f - T * (CurveHeight - 6.0f));
                    }
                    const ImU32 Color = ToColor(Curve.Color);
                    DrawList->AddPolyline(Points, kCurveSamples, Color, 1.5f, ImDrawFlags_None);

                    const char* Name = Curve.Name.c_str();
                    SmallText(DrawList, ImVec2(LegendX, CurveMin.y + CurveHeight + 2.0f), Color, Name);
                    LegendX += SmallTextSize(Name).x + 8.0f;
                }
            }

            if (Playhead.has_value())
            {
                const float X = TimeToX(Playhead.value());
                const float Bottom = CurveCount > 0 ? CurveMin.y + CurveHeight : TrackMax.y;
                DrawList->AddLine(ImVec2(X, TrackMin.y - 2.0f), ImVec2(X, Bottom), kPlayhead, 1.5f);
                DrawList->AddTriangleFilled(ImVec2(X - 4.0f, TrackMin.y - 4.0f), ImVec2(X + 4.0f, TrackMin.y - 4.0f), ImVec2(X, TrackMin.y + 1.0f), kPlayhead);
            }
        }

        void BlendSpace(const CBlendSpace* Space, TOptional<float> X, TOptional<float> Y, float Width)
        {
            const bool bTwoAxes = Space->AxisCount == EBlendSpaceAxes::Two;
            const float Height = bTwoAxes ? Width * 0.62f : 26.0f;
            const float LabelHeight = SmallFontSize() + 2.0f;
            const ImVec2 Min = ReserveBox(ImVec2(Width, Height + LabelHeight));
            const ImVec2 BoxMin = Min;
            const ImVec2 BoxMax(Min.x + Width, Min.y + Height);
            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            DrawList->AddRectFilled(BoxMin, BoxMax, kPlotBackground, kPlotRounding);

            const float Inset = 7.0f;
            const auto ToScreen = [&](const FVector2& Position)
            {
                const float U = Space->AxisX.Normalize(Position.x);
                const float V = bTwoAxes ? Space->AxisY.Normalize(Position.y) : 0.5f;
                return ImVec2(BoxMin.x + Inset + U * (Width - Inset * 2.0f), BoxMax.y - Inset - V * (Height - Inset * 2.0f));
            };

            if (bTwoAxes)
            {
                for (const FBlendSpaceTriangle& Triangle : Space->GetTriangles())
                {
                    DrawList->AddTriangle(ToScreen(Space->Samples[Triangle.A].Position), ToScreen(Space->Samples[Triangle.B].Position),
                        ToScreen(Space->Samples[Triangle.C].Position), kPlotGrid, 1.0f);
                }
            }
            else
            {
                DrawList->AddLine(ImVec2(BoxMin.x + Inset, (BoxMin.y + BoxMax.y) * 0.5f), ImVec2(BoxMax.x - Inset, (BoxMin.y + BoxMax.y) * 0.5f), kPlotGrid, 1.0f);
            }

            FBlendSpaceWeights Weights;
            const FVector2 Input(X.value_or(0.0f), Y.value_or(0.0f));
            if (X.has_value())
            {
                Space->Evaluate(Input, Weights);
            }

            for (int32 SampleIndex = 0; SampleIndex < (int32)Space->Samples.size(); ++SampleIndex)
            {
                float Weight = 0.0f;
                for (int32 Contribution = 0; Contribution < Weights.Count; ++Contribution)
                {
                    Weight += Weights.SampleIndices[Contribution] == SampleIndex ? Weights.Weights[Contribution] : 0.0f;
                }

                const ImVec2 Center = ToScreen(Space->Samples[SampleIndex].Position);
                DrawList->AddCircleFilled(Center, 2.5f, kPlotLabel);
                if (Weight > 1e-3f)
                {
                    DrawList->AddCircleFilled(Center, 3.0f + 6.0f * Weight, EditorColors::ScaleAlpha(EditorColors::U32(EditorColors::Accent()), 0.35f + 0.5f * Weight));
                }
            }

            if (X.has_value())
            {
                const ImVec2 Marker = ToScreen(Input);
                DrawList->AddLine(ImVec2(Marker.x, BoxMin.y + 2.0f), ImVec2(Marker.x, BoxMax.y - 2.0f), IM_COL32(255, 255, 255, 50));
                if (bTwoAxes)
                {
                    DrawList->AddLine(ImVec2(BoxMin.x + 2.0f, Marker.y), ImVec2(BoxMax.x - 2.0f, Marker.y), IM_COL32(255, 255, 255, 50));
                }
                DrawList->AddCircle(Marker, 4.5f, kPlayhead, 0, 1.5f);
            }

            const FFixedString AxisLabel = bTwoAxes
                ? FormatAs<FFixedString>("{} {:.2f}   {} {:.2f}", Space->AxisX.Name.c_str(), Input.x, Space->AxisY.Name.c_str(), Input.y)
                : FormatAs<FFixedString>("{} {:.2f}", Space->AxisX.Name.c_str(), Input.x);
            SmallText(DrawList, ImVec2(Min.x, BoxMax.y + 2.0f), kPlotLabel, X.has_value() ? AxisLabel.c_str() : "No live input");
        }

        void Transfer(float InMin, float InMax, float OutMin, float OutMax, bool bClamp, TOptional<float> Input, float Width)
        {
            const float Height = 60.0f;
            const ImVec2 Min = ReserveBox(ImVec2(Width, Height));
            const ImVec2 Max(Min.x + Width, Min.y + Height);
            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            DrawList->AddRectFilled(Min, Max, kPlotBackground, kPlotRounding);

            const float InSpan = Math::Abs(InMax - InMin) > 1e-5f ? InMax - InMin : 1.0f;
            const float Margin = Math::Abs(InSpan) * 0.25f;
            const float Left = Math::Min(InMin, InMax) - Margin;
            const float Right = Math::Max(InMin, InMax) + Margin;
            const auto Map = [&](float Value)
            {
                const float T = (Value - InMin) / InSpan;
                return OutMin + (bClamp ? Math::Clamp(T, 0.0f, 1.0f) : T) * (OutMax - OutMin);
            };

            const float OutLow = Math::Min(Map(Left), Map(Right));
            const float OutHigh = Math::Max(Map(Left), Map(Right));
            const float OutSpan = OutHigh - OutLow > 1e-5f ? OutHigh - OutLow : 1.0f;
            const float Inset = 6.0f;
            const auto ToScreen = [&](float In, float Out)
            {
                return ImVec2(Min.x + Inset + (In - Left) / (Right - Left) * (Width - Inset * 2.0f),
                              Max.y - Inset - (Out - OutLow) / OutSpan * (Height - Inset * 2.0f));
            };

            const float InLeftX = ToScreen(Math::Min(InMin, InMax), 0.0f).x;
            const float InRightX = ToScreen(Math::Max(InMin, InMax), 0.0f).x;
            DrawList->AddRectFilled(ImVec2(InLeftX, Min.y + 2.0f), ImVec2(InRightX, Max.y - 2.0f), IM_COL32(255, 255, 255, 10));

            ImVec2 Points[kCurveSamples];
            for (int32 Sample = 0; Sample < kCurveSamples; ++Sample)
            {
                const float In = Left + (Right - Left) * (float)Sample / (float)(kCurveSamples - 1);
                Points[Sample] = ToScreen(In, Map(In));
            }
            DrawList->AddPolyline(Points, kCurveSamples, EditorColors::U32(EditorColors::Accent()), 1.5f, ImDrawFlags_None);

            if (Input.has_value())
            {
                const float In = Math::Clamp(Input.value(), Left, Right);
                const ImVec2 Dot = ToScreen(In, Map(Input.value()));
                DrawList->AddCircleFilled(Dot, 3.5f, kPlayhead);
                const FFixedString Label = FormatAs<FFixedString>("{:.2f} > {:.2f}", Input.value(), Map(Input.value()));
                SmallText(DrawList, ImVec2(Min.x + Inset, Min.y + 2.0f), kPlotLabel, Label.c_str());
            }
        }

        void MachineStatus(const FAnimMachineStatus& Status, float Width)
        {
            const float LineHeight = SmallFontSize() + 4.0f;
            const bool bBlending = !Status.From.empty() && Status.Blend < 1.0f;
            const ImVec2 Min = ReserveBox(ImVec2(Width, LineHeight + (bBlending ? 8.0f : 0.0f)));
            ImDrawList* DrawList = ImGui::GetWindowDrawList();

            const FFixedString Label = bBlending
                ? FormatAs<FFixedString>("{} > {}", Status.From.c_str(), Status.Current.c_str())
                : FormatAs<FFixedString>("{}", Status.Current.c_str());
            SmallText(DrawList, Min, EditorColors::U32(EditorColors::Accent()), Label.c_str());

            if (bBlending)
            {
                const ImVec2 BarMin(Min.x, Min.y + LineHeight);
                DrawList->AddRectFilled(BarMin, ImVec2(Min.x + Width, BarMin.y + 5.0f), kPlotBackground, 2.0f);
                DrawList->AddRectFilled(BarMin, ImVec2(Min.x + Width * Math::Clamp(Status.Blend, 0.0f, 1.0f), BarMin.y + 5.0f), EditorColors::U32(EditorColors::Accent()), 2.0f);
            }
        }

        void SeamDecay(float Duration, TOptional<float> Elapsed, float Width)
        {
            const float Height = 70.0f;
            const ImVec2 Min = ReserveBox(ImVec2(Width, Height + SmallFontSize() + 2.0f));
            const ImVec2 Max(Min.x + Width, Min.y + Height);
            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            DrawList->AddRectFilled(Min, Max, kPlotBackground, kPlotRounding);

            const float Inset = 6.0f;
            ImVec2 Points[kCurveSamples];
            for (int32 Sample = 0; Sample < kCurveSamples; ++Sample)
            {
                const float U = (float)Sample / (float)(kCurveSamples - 1);
                Points[Sample] = ImVec2(Min.x + Inset + U * (Width - Inset * 2.0f), Max.y - Inset - InertializationDecay(U) * (Height - Inset * 2.0f));
            }
            DrawList->AddPolyline(Points, kCurveSamples, EditorColors::U32(EditorColors::Accent()), 1.5f, ImDrawFlags_None);

            if (Elapsed.has_value() && Duration > 1e-5f)
            {
                const float U = Math::Clamp(Elapsed.value() / Duration, 0.0f, 1.0f);
                const float X = Min.x + Inset + U * (Width - Inset * 2.0f);
                DrawList->AddLine(ImVec2(X, Min.y + 2.0f), ImVec2(X, Max.y - 2.0f), kPlayhead, 1.0f);
                DrawList->AddCircleFilled(ImVec2(X, Max.y - Inset - InertializationDecay(U) * (Height - Inset * 2.0f)), 3.5f, kPlayhead);
            }

            SmallText(DrawList, ImVec2(Min.x + Inset, Min.y + 2.0f), kPlotLabel, "Offset left");
            const FFixedString End = FormatAs<FFixedString>("{:.2f} s", Duration);
            SmallText(DrawList, ImVec2(Min.x, Max.y + 2.0f), kPlotLabel, "0");
            SmallText(DrawList, ImVec2(Max.x - SmallTextSize(End.c_str()).x, Max.y + 2.0f), kPlotLabel, End.c_str());
        }
    }
}
