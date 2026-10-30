#include <string>
#include "CoreTypeCustomization.h"
#include "imgui.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include <limits>
#include "Core/Math/Math.h"

#include "Core/Math/Transform.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/ImGuiXNumeric.h"

namespace Lumina
{
    namespace
    {
        // Per-axis tag colors (X/Y/Z), matching the gizmo.
        constexpr ImVec4 GAxisColors[3] =
        {
            ImVec4(0.72f, 0.27f, 0.30f, 1.0f),
            ImVec4(0.36f, 0.58f, 0.30f, 1.0f),
            ImVec4(0.24f, 0.44f, 0.78f, 1.0f),
        };
        constexpr const char* GAxisLabels[3] = { "X", "Y", "Z" };

        // Folds a just-drawn item's interaction into the running op, Finished over Started over Updated.
        void AccumulateOp(EPropertyChangeOp& Op)
        {
            if (ImGui::IsItemEdited())                  Op = EPropertyChangeOp::Updated;
            if (ImGui::IsItemActivated())               Op = EPropertyChangeOp::Started;
            if (ImGui::IsItemDeactivatedAfterEdit())    Op = EPropertyChangeOp::Finished;
        }

        // Clicking an axis tag zeroes that component, and UniformLock adds a trailing toggle to the row.
        EPropertyChangeOp DrawAxisRow(const char* ID, const char* Icon, const ImVec4& IconColor, const char* Tooltip, float* Values, float Speed, bool& bResetClicked, float ResetValue = 0.0f, const char* Format = "%.3f", bool* UniformLock = nullptr)
        {
            EPropertyChangeOp Op = EPropertyChangeOp::None;
            const ImGuiStyle& Style = ImGui::GetStyle();
            const float LineHeight = ImGui::GetFrameHeight();
            const float IconColumnW = LineHeight + Style.ItemInnerSpacing.x * 2.0f;

            ImGui::PushID(ID);

            ImGui::AlignTextToFramePadding();
            ImGuiX::TextColoredUnformatted(IconColor, Icon);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGui::SetTooltip("%s", Tooltip);
            }
            ImGui::SameLine(IconColumnW);

            // Reserved on EVERY row, or the lock row's fields end up narrower and break column alignment.
            const float LockW = LineHeight + Style.ItemInnerSpacing.x;
            const float Avail = ImGui::GetContentRegionAvail().x;
            auto FieldWidthFor = [&](float TagWidth) { return (Avail - LockW - 3.0f * (TagWidth + Style.ItemInnerSpacing.x)) / 3.0f; };

            const bool  bCompactTags = FieldWidthFor(LineHeight) < LineHeight * 2.5f;
            const float TagW   = bCompactTags ? Math::Max(3.0f, ImFloor(LineHeight * 0.18f)) : LineHeight;
            const float FieldW = Math::Max(FieldWidthFor(TagW), 1.0f);

            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                ImGui::PushID(Axis);
                if (Axis > 0)
                {
                    ImGui::SameLine(0.0f, Style.ItemInnerSpacing.x);
                }

                ImGui::PushStyleColor(ImGuiCol_Button, GAxisColors[Axis]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GAxisColors[Axis]);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, GAxisColors[Axis]);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
                if (ImGui::Button(bCompactTags ? "##Axis" : GAxisLabels[Axis], ImVec2(TagW, LineHeight)))
                {
                    Values[Axis] = ResetValue;
                    bResetClicked = true;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGui::SetTooltip("Reset %s to %g", GAxisLabels[Axis], ResetValue);
                }

                ImGui::SameLine(0.0f, 0.0f);
                ImGui::SetNextItemWidth(FieldW);
                if (ImGuiX::DragScalar("##V", ImGuiDataType_Float, &Values[Axis], Speed, nullptr, nullptr, Format))
                {
                    Op = EPropertyChangeOp::Updated;
                }
                AccumulateOp(Op);
                ImGui::PopID();
            }

            if (UniformLock != nullptr)
            {
                ImGui::SameLine(0.0f, Style.ItemInnerSpacing.x);
                if (ImGui::Button(*UniformLock ? LE_ICON_LOCK : LE_ICON_LOCK_OPEN_VARIANT, ImVec2(LineHeight, LineHeight)))
                {
                    *UniformLock = !*UniformLock;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGui::SetTooltip("%s", *UniformLock
                        ? "Uniform: dragging one axis scales the others by the same ratio."
                        : "Non-uniform: each axis is independent.");
                }
            }

            ImGui::PopID();
            return Op;
        }
    }

    EPropertyChangeOp FVec2PropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        FStructProperty* Prop = static_cast<FStructProperty*>(Property->Property);

        const float Min = Prop->GetMetadataNumber<float>("ClampMin").value_or(0.0f);
        const float Max = Prop->GetMetadataNumber<float>("ClampMax").value_or(0.0f);

        float Speed = ResolveDragSpeed(Prop, 0.01f);
        const FString UnitFormat = BuildUnitFormat(Prop, "%.3f");
        const char* Format = UnitFormat.empty() ? "%.3f" : UnitFormat.c_str();
        if (Prop->HasEditorFlag(EPropertyEditorFlags::NoDrag))
        {
            // An UNSCALED base, so the plus and minus buttons step the same amount every press.
            float Step = ResolveBaseStep(Prop, 0.01f);
            ImGuiX::InputScalarN("##", ImGuiDataType_Float, Math::ValuePtr(DisplayValue), 2, &Step, nullptr, Format);
        }
        else
        {
            ImGuiX::DragFloat2("##", Math::ValuePtr(DisplayValue), Speed, Min, Max, Format);
        }

        ImGui::PopItemWidth();

        EPropertyChangeOp Result = EPropertyChangeOp::None;
        if (ImGui::IsItemEdited())
        {
            Result = EPropertyChangeOp::Updated;
        }
        if (ImGui::IsItemActivated())
        {
            Result = EPropertyChangeOp::Started;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            Result = EPropertyChangeOp::Finished;
        }
        
        return Result;
    }

    void FVec2PropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FVec2PropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FVector2 ActualValue;
        Property->GetValue(&ActualValue);
        
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }

    EPropertyChangeOp FVec3PropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        FStructProperty* Prop = static_cast<FStructProperty*>(Property->Property);

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        if (Prop->HasEditorFlag(EPropertyEditorFlags::Color))
        {
            ImGui::ColorEdit3("##", Math::ValuePtr(DisplayValue));
        }
        else
        {
            const float Min = Prop->GetMetadataNumber<float>("ClampMin").value_or(0.0f);
            const float Max = Prop->GetMetadataNumber<float>("ClampMax").value_or(0.0f);

            float Speed = ResolveDragSpeed(Prop, 0.01f);
            const FString UnitFormat = BuildUnitFormat(Prop, "%.3f");
            const char* Format = UnitFormat.empty() ? "%.3f" : UnitFormat.c_str();
            if (Prop->HasEditorFlag(EPropertyEditorFlags::NoDrag))
            {
                // An UNSCALED base, so the plus and minus buttons step the same amount every press.
                float Step = ResolveBaseStep(Prop, 0.01f);
                ImGuiX::InputScalarN("##", ImGuiDataType_Float, Math::ValuePtr(DisplayValue), 3, &Step, nullptr, Format);
            }
            else
            {
                ImGuiX::DragFloat3("##", Math::ValuePtr(DisplayValue), Speed, Min, Max, Format);
            }
        }
        
        ImGui::PopItemWidth();

        EPropertyChangeOp Result = EPropertyChangeOp::None;
        if (ImGui::IsItemEdited())
        {
            Result = EPropertyChangeOp::Updated;
        }
        if (ImGui::IsItemActivated())
        {
            Result = EPropertyChangeOp::Started;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            Result = EPropertyChangeOp::Finished;
        }
        
        return Result;
    }
    
    void FVec3PropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FVec3PropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FVector3 ActualValue;
        Property->GetValue(&ActualValue);
        
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }

    EPropertyChangeOp FVec4PropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        FStructProperty* Prop = static_cast<FStructProperty*>(Property->Property);

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        if (Prop->HasEditorFlag(EPropertyEditorFlags::Color))
        {
            ImGui::ColorEdit4("##", Math::ValuePtr(DisplayValue));
        }
        else
        {
            const float Min = Prop->GetMetadataNumber<float>("ClampMin").value_or(0.0f);
            const float Max = Prop->GetMetadataNumber<float>("ClampMax").value_or(0.0f);

            float Speed = ResolveDragSpeed(Prop, 0.01f);
            const FString UnitFormat = BuildUnitFormat(Prop, "%.3f");
            const char* Format = UnitFormat.empty() ? "%.3f" : UnitFormat.c_str();
            if (Prop->HasEditorFlag(EPropertyEditorFlags::NoDrag))
            {
                // An UNSCALED base, so the plus and minus buttons step the same amount every press.
                float Step = ResolveBaseStep(Prop, 0.01f);
                ImGuiX::InputScalarN("##", ImGuiDataType_Float, Math::ValuePtr(DisplayValue), 4, &Step, nullptr, Format);
            }
            else
            {
                ImGuiX::DragFloat4("##", Math::ValuePtr(DisplayValue), Speed, Min, Max, Format);
            }
        }

        ImGui::PopItemWidth();
        
        EPropertyChangeOp Result = EPropertyChangeOp::None;
        if (ImGui::IsItemEdited())
        {
            Result = EPropertyChangeOp::Updated;
        }
        if (ImGui::IsItemActivated())
        {
            Result = EPropertyChangeOp::Started;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            Result = EPropertyChangeOp::Finished;
        }
        
        return Result;
    }

    void FVec4PropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FVec4PropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FVector4 ActualValue;
        Property->GetValue(&ActualValue);
        
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }

    EPropertyChangeOp FQuatPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        ImGuiX::DragFloat4("##", Math::ValuePtr(DisplayValue), 0.01f * MouseAdaptiveDragScale());

        ImGui::PopItemWidth();
        
        EPropertyChangeOp Result = EPropertyChangeOp::None;
        if (ImGui::IsItemEdited())
        {
            Result = EPropertyChangeOp::Updated;
        }
        if (ImGui::IsItemActivated())
        {
            Result = EPropertyChangeOp::Started;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            Result = EPropertyChangeOp::Finished;
        }
        
        return Result;
    }

    void FQuatPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FQuatPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FQuat ActualValue;
        Property->GetValue(&ActualValue);
        
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }
    
    EPropertyChangeOp FTransformPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        EPropertyChangeOp DragOp = EPropertyChangeOp::None;
        auto Merge = [&DragOp](EPropertyChangeOp Op)
        {
            if (Op != EPropertyChangeOp::None)
            {
                DragOp = Op;
            }
        };

        bool bReset = false;
        // The SIMD transform has no scalar member to point at, so edit scalar buffers and write back.
        FVector3 Translation = DisplayValue.GetLocation();
        Merge(DrawAxisRow("T", LE_ICON_AXIS_ARROW, ImVec4(0.40f, 0.70f, 1.0f, 1.0f), "Translation (Location)", Math::ValuePtr(Translation), 0.01f * MouseAdaptiveDragScale(), bReset, 0.0f, "%.3f m"));
        DisplayValue.SetLocation(Translation);

        const FQuat CurrentRotation = DisplayValue.GetRotation();
        if (!Math::IsSameRotation(CurrentRotation, EulerRotationSource))
        {
            EulerDegrees = Math::Degrees(Math::EulerAngles(CurrentRotation));
            EulerRotationSource = CurrentRotation;
        }

        bool bRotationReset = false;
        const FVector3 PreviousEuler = EulerDegrees;
        const EPropertyChangeOp RotationOp = DrawAxisRow("R", LE_ICON_ROTATE_360, ImVec4(0.40f, 1.0f, 0.70f, 1.0f), "Rotation (Euler Angles)", Math::ValuePtr(EulerDegrees), 0.1f * MouseAdaptiveDragScale(), bRotationReset, 0.0f, "%.3f\xc2\xb0");
        // A typed value lands on the frame the field deactivates, which reports Finished rather than Updated.
        if (EulerDegrees != PreviousEuler || bRotationReset)
        {
            DisplayValue.SetRotationFromEuler(EulerDegrees);
            EulerRotationSource = DisplayValue.GetRotation();
        }
        Merge(RotationOp);
        bReset |= bRotationReset;

        FVector3 ScaleVec = DisplayValue.GetScale();
        const FVector3 PrevScale = ScaleVec;
        bool bScaleReset = false;
        const EPropertyChangeOp ScaleOp = DrawAxisRow("S", LE_ICON_ARROW_TOP_RIGHT_BOTTOM_LEFT, ImVec4(1.0f, 0.70f, 0.40f, 1.0f),
            "Scale (multiplier)", Math::ValuePtr(ScaleVec), 0.01f * MouseAdaptiveDragScale(), bScaleReset, 1.0f, "%.3f\xc3\x97", &bUniformScale);

        if (bUniformScale)
        {
            // Propagated by ratio, since scale multiplies, and a previous zero has no ratio to carry.
            float* Scale = Math::ValuePtr(ScaleVec);
            const float* Prev = Math::ValuePtr(PrevScale);
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                if (Math::IsNearlyEqual(Scale[Axis], Prev[Axis]))
                {
                    continue;
                }

                const float Ratio = Prev[Axis] != 0.0f ? (Scale[Axis] / Prev[Axis]) : 0.0f;
                for (int32 Other = 0; Other < 3; ++Other)
                {
                    if (Other != Axis)
                    {
                        Scale[Other] = Ratio != 0.0f ? Prev[Other] * Ratio : Scale[Axis];
                    }
                }
                break;   // only one axis can be dragged at a time
            }
        }

        // Locked, an axis reset means "reset the scale", since the axes are not independent.
        if (bUniformScale && bScaleReset)
        {
            ScaleVec = FVector3(1.0f);
        }

        Merge(ScaleOp);
        bReset |= bScaleReset;
        DisplayValue.SetScale(ScaleVec);

        // A reset writes this frame, so open the transaction now and commit it next, like a discrete edit.
        if (bReset)
        {
            if (bFinishPending)
            {
                return EPropertyChangeOp::Updated;
            }
            bFinishPending = true;
            return EPropertyChangeOp::Started;
        }
        if (bFinishPending)
        {
            bFinishPending = false;
            return EPropertyChangeOp::Finished;
        }

        return DragOp;
    }

    void FTransformPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FTransformPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FTransform ActualValue;
        Property->GetValue(&ActualValue);
        
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }
}
