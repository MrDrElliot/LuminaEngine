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

        // Tracks a just-drawn item and folds its interaction into the running op
        // (Finished > Started > Updated, matching the rest of the customizations).
        void AccumulateOp(EPropertyChangeOp& Op)
        {
            if (ImGui::IsItemEdited())                  Op = EPropertyChangeOp::Updated;
            if (ImGui::IsItemActivated())               Op = EPropertyChangeOp::Started;
            if (ImGui::IsItemDeactivatedAfterEdit())    Op = EPropertyChangeOp::Finished;
        }

        // One labeled row: leading category icon + three color-tagged XYZ drag fields.
        // Clicking an axis tag zeroes that component and sets bResetClicked.
        // UniformLock, when non-null, adds a trailing toggle to the row and reserves width for it. The
        // caller owns what "uniform" means for its values; this only draws and stores the flag.
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

            const float TagW = LineHeight;
            // Reserved on EVERY row, not only the one that owns a lock. The width comes out of the three
            // fields, so charging it to the scale row alone made that row's fields narrower than the two
            // above it and broke the column alignment down the whole widget.
            const float LockW = LineHeight + Style.ItemInnerSpacing.x;
            const float Avail = ImGui::GetContentRegionAvail().x;
            const float FieldW = Math::Max((Avail - LockW - 3.0f * (TagW + Style.ItemInnerSpacing.x)) / 3.0f, 1.0f);

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
                if (ImGui::Button(GAxisLabels[Axis], ImVec2(TagW, LineHeight)))
                {
                    Values[Axis] = ResetValue;
                    bResetClicked = true;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGui::SetTooltip("Reset to: %f", ResetValue);
                }

                ImGui::SameLine(0.0f, 0.0f);
                ImGui::SetNextItemWidth(FieldW);
                if (ImGui::DragScalar("##V", ImGuiDataType_Float, &Values[Axis], Speed, nullptr, nullptr, Format))
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

    EPropertyChangeOp FVec2PropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        FStructProperty* Prop = static_cast<FStructProperty*>(Property->Property);

        TOptional<float> MinOpt;
        TOptional<float> MaxOpt;

        if (Prop->HasMetadata("ClampMin"))
        {
            MinOpt = std::stof(Prop->GetMetadata("ClampMin").c_str());
        }
        if (Prop->HasMetadata("ClampMax"))
        {
            MaxOpt = std::stof(Prop->GetMetadata("ClampMax").c_str());
        }

        float Min = MinOpt ? MinOpt.value() : 0.0f;
        float Max = MaxOpt ? MaxOpt.value() : 0.0f;

        float Speed = ResolveDragSpeed(Prop, 0.01f);
        const FString UnitFormat = BuildUnitFormat(Prop, "%.3f");
        const char* Format = UnitFormat.empty() ? "%.3f" : UnitFormat.c_str();
        if (Prop->HasMetadata("NoDrag"))
        {
            // UNSCALED base: the +/- buttons must step the same amount every press.
            float Step = ResolveBaseStep(Prop, 0.01f);
            ImGui::InputScalarN("##", ImGuiDataType_Float, Math::ValuePtr(DisplayValue), 2, &Step, nullptr, Format);
        }
        else
        {
            ImGui::DragFloat2("##", Math::ValuePtr(DisplayValue), Speed, Min, Max, Format);
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

    EPropertyChangeOp FVec3PropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        FStructProperty* Prop = static_cast<FStructProperty*>(Property->Property);

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        if (Prop->Metadata.HasMetadata("Color"))
        {
            ImGui::ColorEdit3("##", Math::ValuePtr(DisplayValue));
        }
        else
        {
            TOptional<float> MinOpt;
            TOptional<float> MaxOpt;

            if (Prop->HasMetadata("ClampMin"))
            {
                MinOpt = std::stof(Prop->GetMetadata("ClampMin").c_str());
            }
            if (Prop->HasMetadata("ClampMax"))
            {
                MaxOpt = std::stof(Prop->GetMetadata("ClampMax").c_str());
            }

            float Min = MinOpt ? MinOpt.value() : 0.0f;
            float Max = MaxOpt ? MaxOpt.value() : 0.0f;

            float Speed = ResolveDragSpeed(Prop, 0.01f);
            const FString UnitFormat = BuildUnitFormat(Prop, "%.3f");
            const char* Format = UnitFormat.empty() ? "%.3f" : UnitFormat.c_str();
            if (Prop->HasMetadata("NoDrag"))
            {
                // UNSCALED base: the +/- buttons must step the same amount every press.
                float Step = ResolveBaseStep(Prop, 0.01f);
                ImGui::InputScalarN("##", ImGuiDataType_Float, Math::ValuePtr(DisplayValue), 3, &Step, nullptr, Format);
            }
            else
            {
                ImGui::DragFloat3("##", Math::ValuePtr(DisplayValue), Speed, Min, Max, Format);
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

    EPropertyChangeOp FVec4PropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        FStructProperty* Prop = static_cast<FStructProperty*>(Property->Property);

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        if (Prop->Metadata.HasMetadata("Color"))
        {
            ImGui::ColorEdit4("##", Math::ValuePtr(DisplayValue));
        }
        else
        {
            TOptional<float> MinOpt;
            TOptional<float> MaxOpt;

            if (Prop->HasMetadata("ClampMin"))
            {
                MinOpt = std::stof(Prop->GetMetadata("ClampMin").c_str());
            }
            if (Prop->HasMetadata("ClampMax"))
            {
                MaxOpt = std::stof(Prop->GetMetadata("ClampMax").c_str());
            }

            float Min = MinOpt ? MinOpt.value() : 0.0f;
            float Max = MaxOpt ? MaxOpt.value() : 0.0f;

            float Speed = ResolveDragSpeed(Prop, 0.01f);
            const FString UnitFormat = BuildUnitFormat(Prop, "%.3f");
            const char* Format = UnitFormat.empty() ? "%.3f" : UnitFormat.c_str();
            if (Prop->HasMetadata("NoDrag"))
            {
                // UNSCALED base: the +/- buttons must step the same amount every press.
                float Step = ResolveBaseStep(Prop, 0.01f);
                ImGui::InputScalarN("##", ImGuiDataType_Float, Math::ValuePtr(DisplayValue), 4, &Step, nullptr, Format);
            }
            else
            {
                ImGui::DragFloat4("##", Math::ValuePtr(DisplayValue), Speed, Min, Max, Format);
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

    EPropertyChangeOp FQuatPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        ImGui::DragFloat4("##", Math::ValuePtr(DisplayValue), 0.01f * MouseAdaptiveDragScale());

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
    
    EPropertyChangeOp FTransformPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
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
        // Translation is always in meters; rotation (degrees) and scale stay unitless.
        // The SIMD transform has no scalar member to point at, so edit scalar buffers and write back.
        FVector3 Translation = DisplayValue.GetLocation();
        Merge(DrawAxisRow("T", LE_ICON_AXIS_ARROW, ImVec4(0.40f, 0.70f, 1.0f, 1.0f), "Translation (Location)", Math::ValuePtr(Translation), 0.01f * MouseAdaptiveDragScale(), bReset, 0.0f, "%.3f m"));
        DisplayValue.SetLocation(Translation);

        FVector3 EulerRotation = Math::Degrees(Math::EulerAngles(DisplayValue.GetRotation()));
        bool bRotationReset = false;
        const EPropertyChangeOp RotationOp = DrawAxisRow("R", LE_ICON_ROTATE_360, ImVec4(0.40f, 1.0f, 0.70f, 1.0f), "Rotation (Euler Angles)", Math::ValuePtr(EulerRotation), 0.1f * MouseAdaptiveDragScale(), bRotationReset, 0.0f, "%.3f\xc2\xb0");
        if (RotationOp == EPropertyChangeOp::Updated || bRotationReset)
        {
            DisplayValue.SetRotationFromEuler(EulerRotation);
        }
        Merge(RotationOp);
        bReset |= bRotationReset;

        FVector3 ScaleVec = DisplayValue.GetScale();
        const FVector3 PrevScale = ScaleVec;
        bool bScaleReset = false;
        const EPropertyChangeOp ScaleOp = DrawAxisRow("S", LE_ICON_ARROW_TOP_RIGHT_BOTTOM_LEFT, ImVec4(1.0f, 0.70f, 0.40f, 1.0f),
            "Scale (multiplier)", Math::ValuePtr(ScaleVec), 0.01f * MouseAdaptiveDragScale(), bScaleReset, 1.0f, "%.3f\xc3\x97", &bUniformScale);

        if (bUniformScale && ScaleOp == EPropertyChangeOp::Updated)
        {
            // Propagate by RATIO, not by delta: scale is a multiplier, so dragging X from 1 to 2 should
            // double the other two, not add 1 to them. A previous value of zero has no ratio to carry,
            // so those axes take the edited value outright rather than staying stuck at zero.
            float* Scale = Math::ValuePtr(ScaleVec);
            const float* Prev = Math::ValuePtr(PrevScale);
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                if (Scale[Axis] == Prev[Axis])
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

        // A reset writes the value this frame: open the undo transaction now (Started),
        // commit it next frame (Finished), like the discrete object/array edits.
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
