#include "CoreTypeCustomization.h"
#include "imgui.h"
#include "Containers/FunctionRef.h"
#include "Containers/StringFormat.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Physics/PhysicsTypes.h"
#include "Renderer/RenderLayers.h"

namespace Lumina
{
    namespace
    {
        // A layer's label, false when nobody named it so the combo lists it only while it is set.
        using FLayerLabelFn = TFunctionRef<bool(uint32 Layer, FFixedString& OutLabel)>;

        FFixedString MaskPreview(uint32 Bits, uint32 AllBits, uint32 LayerCount, FLayerLabelFn Label)
        {
            if (Bits == AllBits)
            {
                return FFixedString("Everything");
            }
            if (Bits == 0u)
            {
                return FFixedString("Nothing");
            }

            // Leaves room for the ellipsis, since the fixed string cannot grow past its capacity.
            constexpr SIZE_T MaxPreview = 200;
            FFixedString Preview;
            for (uint32 Layer = 0; Layer < LayerCount; ++Layer)
            {
                if ((Bits & (1u << Layer)) == 0u)
                {
                    continue;
                }
                FFixedString Name;
                Label(Layer, Name);
                if (Preview.size() + Name.size() + 2 > MaxPreview)
                {
                    Preview += ", ...";
                    break;
                }
                if (!Preview.empty())
                {
                    Preview += ", ";
                }
                Preview += Name.c_str();
            }
            return Preview;
        }

        bool DrawLayerMaskCombo(uint32& Bits, uint32 LayerCount, FLayerLabelFn Label)
        {
            const uint32 AllBits = LayerCount >= 32 ? 0xFFFFFFFFu : (1u << LayerCount) - 1u;
            bool bChanged = false;

            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo("##layers", MaskPreview(Bits, AllBits, LayerCount, Label).c_str(), ImGuiComboFlags_HeightLarge))
            {
                if (ImGui::Selectable("Everything", Bits == AllBits))
                {
                    Bits     = AllBits;
                    bChanged = true;
                }
                if (ImGui::Selectable("Nothing", Bits == 0u))
                {
                    Bits     = 0u;
                    bChanged = true;
                }
                ImGui::Separator();

                for (uint32 Layer = 0; Layer < LayerCount; ++Layer)
                {
                    const uint32 Bit = 1u << Layer;
                    FFixedString Name;
                    if (!Label(Layer, Name) && (Bits & Bit) == 0u)
                    {
                        continue;
                    }

                    bool bSet = (Bits & Bit) != 0u;
                    ImGui::PushID((int)Layer);
                    if (ImGui::Checkbox(Name.c_str(), &bSet))
                    {
                        Bits     = bSet ? (Bits | Bit) : (Bits & ~Bit);
                        bChanged = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
            return bChanged;
        }

        bool RenderLayerLabel(uint32 Layer, FFixedString& OutLabel)
        {
            const FName Name = SRenderLayerMask::GetLayerName((int32)Layer);
            if (Name == NAME_None)
            {
                AppendFormat(OutLabel, "Layer {}", Layer);
                return false;
            }
            OutLabel = Name.c_str();
            return true;
        }

        bool CollisionLayerLabel(uint32 Layer, FFixedString& OutLabel)
        {
            const FName Name = SCollisionProfile::GetLayerName((int32)Layer);
            if (Name == NAME_None)
            {
                AppendFormat(OutLabel, "Channel {}", Layer - CollisionLayers::kFirstChannel);
                return false;
            }
            OutLabel = Name.c_str();
            return true;
        }
    }

    EPropertyChangeOp FRenderLayerMaskPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        return EditSession.Advance(DrawLayerMaskCombo(CachedBits, RenderLayers::kCount, RenderLayerLabel), false);
    }

    void FRenderLayerMaskPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->SetValue(SRenderLayerMask(CachedBits));
    }

    void FRenderLayerMaskPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        SRenderLayerMask Value;
        Property->GetValue(&Value);
        CachedBits = Value.Bits;
    }

    EPropertyChangeOp FCollisionLayersPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        return EditSession.Advance(DrawLayerMaskCombo(CachedBits, CollisionLayers::kCount, CollisionLayerLabel), false);
    }

    void FCollisionLayersPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        static_cast<FEnumProperty*>(Property->Property)->GetInnerProperty()->SetIntPropertyValue(Property->GetValuePtr(), (int64)CachedBits);
    }

    void FCollisionLayersPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedBits = (uint32)static_cast<FEnumProperty*>(Property->Property)->GetInnerProperty()->GetSignedIntPropertyValue(Property->GetValuePtr());
    }
}
