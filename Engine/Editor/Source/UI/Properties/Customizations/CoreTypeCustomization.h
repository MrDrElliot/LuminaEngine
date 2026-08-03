#pragma once
#include "imgui.h"
#include "Core/Math/Transform.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Math/Math.h"
#include "Input/Key.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"

namespace Lumina
{
    static bool IsFloatType(ImGuiDataType dt)
    {
        return dt == ImGuiDataType_Float || dt == ImGuiDataType_Double;
    }

    // Default printf format ImGui uses for each scalar type, so we can append a
    // unit suffix without losing the type's natural precision.
    inline const char* DefaultScalarFormat(ImGuiDataType dt)
    {
        switch (dt)
        {
        case ImGuiDataType_Float:
        case ImGuiDataType_Double: return "%.3f";
        case ImGuiDataType_S64:    return "%lld";
        case ImGuiDataType_U64:    return "%llu";
        case ImGuiDataType_U8:
        case ImGuiDataType_U16:
        case ImGuiDataType_U32:    return "%u";
        default:                   return "%d";
        }
    }

    // If the property carries a "Units" metadata, returns a printf format that
    // renders the value followed by the unit (e.g. "%.3f m"); otherwise returns
    // an empty string, signaling the caller to pass nullptr (ImGui default).
    // The unit's '%' chars are escaped so a "%" unit can't corrupt the format.
    inline FString BuildUnitFormat(const FProperty* Prop, const char* BaseFormat)
    {
        if (!Prop->HasMetadata("Units"))
        {
            return FString();
        }

        FString Unit = Prop->GetMetadata("Units").c_str();
        if (Unit.empty())
        {
            return FString();
        }

        FString Format = BaseFormat;
        Format.append(" ");
        for (char c : Unit)
        {
            if (c == '%') Format.append("%%");
            else          Format.push_back(c);
        }
        return Format;
    }

    template<typename T, ImGuiDataType_ DT>
    class FNumericPropertyCustomization : public IPropertyTypeCustomization
    {
        using ValueType = T;
        
    public:
        
        static TSharedPtr<FNumericPropertyCustomization> MakeInstance()
        {
            return MakeShared<FNumericPropertyCustomization>();
        }
        
        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override
        {
            FProperty* Prop = Property->Property;
            float Speed = Prop->HasMetadata("Delta") ? std::stof(Prop->GetMetadata("Delta").c_str()) : (IsFloatType(DT) ? 0.01f : 1.0f);
            
            TOptional<ValueType> MinOpt;
            TOptional<ValueType> MaxOpt;

            if (Prop->HasMetadata("ClampMin"))
            {
                MinOpt = static_cast<ValueType>(std::stod(Prop->GetMetadata("ClampMin").c_str()));
            }
            if (Prop->HasMetadata("ClampMax"))
            {
                MaxOpt = static_cast<ValueType>(std::stod(Prop->GetMetadata("ClampMax").c_str()));
            }

            const ValueType* Min = MinOpt ? &MinOpt.value() : nullptr;
            const ValueType* Max = MaxOpt ? &MaxOpt.value() : nullptr;

            const FString UnitFormat = BuildUnitFormat(Prop, DefaultScalarFormat(DT));
            const char* Format = UnitFormat.empty() ? nullptr : UnitFormat.c_str();

            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
            if (Prop->HasMetadata("NoDrag"))
            {
                // Type-only entry; +/- buttons step by Delta, no click-drag scrubbing.
                ValueType Step = static_cast<ValueType>(Speed);
                ImGui::InputScalar("##Value", DT, &DisplayValue, &Step, nullptr, Format);
                if (Min && DisplayValue < *Min)
                {
                    DisplayValue = *Min;
                }
                if (Max && DisplayValue > *Max)
                {
                    DisplayValue = *Max;
                }
            }
            else
            {
                ImGui::DragScalar("##Value", DT, &DisplayValue, Speed, Min, Max, Format);
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
        
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override
        {
            CachedValue = DisplayValue;
            Property->SetValue(CachedValue);
        }

        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override
        {
            ValueType ActualValue;
            Property->GetValue(&ActualValue);
        
            if (!Math::IsNearlyEqual(CachedValue, ActualValue, LE_SMALL_NUMBER))
            {
                CachedValue = DisplayValue = ActualValue;
            }
        }

        ValueType CachedValue;
        ValueType DisplayValue;
        
    };
    
    class FBoolPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FBoolPropertyCustomization> MakeInstance()
        {
            return MakeShared<FBoolPropertyCustomization>();
        }
        
        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::Checkbox("##", &bValue);
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
        
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override
        {
            Property->SetValue(bValue);
        }

        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override
        {
            Property->GetValue(&bValue);
        }

        bool bValue;
    };
    
    class FCObjectPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FCObjectPropertyCustomization> MakeInstance()
        {
            return MakeShared<FCObjectPropertyCustomization>();
        }
        
        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;

        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        TWeakObjectPtr<CObject> Object;
        ImGuiTextFilter SearchFilter;

        // Object edits (clear/pick/drop) are one-frame events: emit Started on the change
        // frame, Finished the next, so they form a proper undo transaction.
        bool bFinishPending = false;
    };

    // Soft-object asset picker: identical UX to FCObjectPropertyCustomization but edits the
    // FSoftObjectPath by string path and never loads the asset (discovers via FAssetRegistry).
    class FSoftObjectPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FSoftObjectPropertyCustomization> MakeInstance()
        {
            return MakeShared<FSoftObjectPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        FSoftObjectPath Path;
        ImGuiTextFilter SearchFilter;
        bool bFinishPending = false;
    };

    // TSubclassOf<T> picker: searchable dropdown over every CClass that is MetaClass (T) or derived.
    // Stores the chosen CClass* directly (the TSubclassOf member is a single CClass* at offset 0).
    class FClassPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FClassPropertyCustomization> MakeInstance()
        {
            return MakeShared<FClassPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        CClass* Value = nullptr;
    };

    // TSubStructOf<T> picker: the struct analog of FClassPropertyCustomization.
    class FSubStructPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FSubStructPropertyCustomization> MakeInstance()
        {
            return MakeShared<FSubStructPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        CStruct* Value = nullptr;
    };

    class FEnumPropertyCustomization : public IPropertyTypeCustomization
    {
    public:
        
        static TSharedPtr<FEnumPropertyCustomization> MakeInstance()
        {
            return MakeShared<FEnumPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        int64 CachedValue = 0;
    };

    class FNamePropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FNamePropertyCustomization> MakeInstance()
        {
            return MakeShared<FNamePropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        FName CachedValue;
        FName DisplayValue;
        ImGuiTextFilter SearchFilter;   // input-action picker
        ImGuiTextFilter BoneFilter;
        FTreeListView   BoneTree;
        int32           LastBuiltBoneCount = 0;
    };

    class FStringPropertyCustomization : public IPropertyTypeCustomization
    {
    public:
        
        static TSharedPtr<FStringPropertyCustomization> MakeInstance()
        {
            return MakeShared<FStringPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        FString CachedValue;
        FString DisplayValue;
        ImGuiTextFilter SearchFilter;
    };

    class FVec2PropertyCustomization : public IPropertyTypeCustomization
    {
    public:
        
        static TSharedPtr<FVec2PropertyCustomization> MakeInstance()
        {
            return MakeShared<FVec2PropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        FVector2 CachedValue{};
        FVector2 DisplayValue{};
    };

    class FVec3PropertyCustomization : public IPropertyTypeCustomization
    {
    public:
        
        static TSharedPtr<FVec3PropertyCustomization> MakeInstance()
        {
            return MakeShared<FVec3PropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:
        
        FVector3 CachedValue{};
        FVector3 DisplayValue{};
    };

    class FVec4PropertyCustomization : public IPropertyTypeCustomization
    {
    public:
        
        static TSharedPtr<FVec4PropertyCustomization> MakeInstance()
        {
            return MakeShared<FVec4PropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        FVector4 CachedValue{};
        FVector4 DisplayValue{};
    };

    class FQuatPropertyCustomization : public IPropertyTypeCustomization
    {
    public:
        
        static TSharedPtr<FQuatPropertyCustomization> MakeInstance()
        {
            return MakeShared<FQuatPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        FQuat CachedValue{};
        FQuat DisplayValue{};
    };

    class FKeyPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FKeyPropertyCustomization> MakeInstance()
        {
            return MakeShared<FKeyPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        SKey CachedValue;
        SKey DisplayValue;

        // True while listening for the bind key. bArmed skips the frame the activating click landed on,
        // so the click that starts the capture isn't itself bound.
        bool bCapturing = false;
        bool bArmed = false;

        // Capture / clear are one-frame discrete edits: Started this frame, Finished the next, so they
        // form a proper undo transaction (mirrors the object / transform-reset customizations).
        bool bFinishPending = false;
    };

    class FTransformPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FTransformPropertyCustomization> MakeInstance()
        {
            return MakeShared<FTransformPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

        FTransform CachedValue{};
        FTransform DisplayValue{};

        // Clicking an axis tag resets that component to 0, a discrete edit, so we open the
        // transaction the click frame (Started) and commit it the next (Finished).
        bool bFinishPending = false;

        // Scale-row lock: edits carry to the other two axes by ratio. Session-only, per widget.
        bool bUniformScale = false;
    };

}
