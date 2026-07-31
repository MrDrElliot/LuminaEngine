#include "MaterialNode_CurveSample.h"

#include "Core/Math/Math.h"
#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    void CMaterialExpression_CurveSample::BuildNode()
    {
        Super::BuildNode();

        Output->SetPinName("Value");
        Output->SetInputType(EMaterialInputType::Float);
        Output->SetComponentMask(EComponentMask::R);

        Time = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Time", ENodePinDirection::Input));
        Time->SetPinName("Time");
        Time->SetInputType(EMaterialInputType::Float);
        Time->SetHideDuringConnection(false);
    }

    void CMaterialExpression_CurveSample::SetNodeValue(void* Value)
    {
        Curve = (CCurveAsset*)Value;
    }

    void CMaterialExpression_CurveSample::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        // No curve still has to produce a value so downstream code compiles.
        if (!Curve.IsValid())
        {
            Compiler.NumericConstant(FullName, 0.0f);
            return;
        }

        Compiler.CurveSample(FullName, Curve->Curve, Time);
    }

    void CMaterialExpression_CurveSample::DrawNodeBody()
    {
        const ImVec2 PreviewSize(120.0f, 60.0f);
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const ImVec2 Extent = ImVec2(Origin.x + PreviewSize.x, Origin.y + PreviewSize.y);

        ImGui::Dummy(PreviewSize);

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawList->AddRectFilled(Origin, Extent, IM_COL32(18, 18, 22, 255), 3.0f);
        DrawList->AddRect(Origin, Extent, IM_COL32(70, 70, 80, 255), 3.0f);

        if (!Curve.IsValid() || Curve->Curve.IsEmpty())
        {
            return;
        }

        float MinTime = 0.0f;
        float MaxTime = 0.0f;
        float MinValue = 0.0f;
        float MaxValue = 0.0f;
        Curve->Curve.GetTimeRange(MinTime, MaxTime);
        Curve->Curve.GetValueRange(MinValue, MaxValue);

        const float TimeSpan = Math::Max(MaxTime - MinTime, 1e-4f);
        const float ValueSpan = Math::Max(MaxValue - MinValue, 1e-4f);

        constexpr int32 NumSamples = 48;
        ImVec2 Points[NumSamples];
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            const float Alpha = (float)Index / (float)(NumSamples - 1);
            const float Sample = Curve->Curve.Evaluate(MinTime + Alpha * TimeSpan);
            const float Normalized = Math::Clamp((Sample - MinValue) / ValueSpan, 0.0f, 1.0f);

            Points[Index] = ImVec2(Origin.x + 3.0f + Alpha * (PreviewSize.x - 6.0f), Extent.y - 3.0f - Normalized * (PreviewSize.y - 6.0f));
        }

        DrawList->AddPolyline(Points, NumSamples, IM_COL32(255, 190, 60, 255), ImDrawFlags_None, 1.5f);
    }
}
