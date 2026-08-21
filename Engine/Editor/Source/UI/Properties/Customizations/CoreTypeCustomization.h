#pragma once
#include <string>
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
    class CAnimationGraph;

    static bool IsFloatType(ImGuiDataType dt)
    {
        return dt == ImGuiDataType_Float || dt == ImGuiDataType_Double;
    }

    /**
     * Multiplier for a drag widget's step, taken from how fast the cursor is currently moving.
     *
     * ImGui moves a drag value by (pixels moved * speed), so a single fixed speed has to serve both
     * "nudge this by 0.01" and "sweep this to 5000" -- and it cannot. Folding the cursor's SPEED into
     * the step lets one widget do both: inch the mouse and the step falls below the base for fine
     * tuning, throw it and the step grows superlinearly enough to cross a large range in one drag.
     *
     * Deliberately NOT applied when the property authored an explicit Delta. That value is a statement
     * about the field's units (a "Density" that means something at 0.0001), and scaling it would make
     * the annotation meaningless.
     *
     * Composes with ImGui's own modifiers, which is the point: Alt still gives a slow precise drag and
     * Shift a fast one, on top of whatever the cursor speed is doing.
     */
    inline float MouseAdaptiveDragScale()
    {
        // Horizontal only -- that is the axis ImGui's drag behavior reads.
        const float Pixels = Math::Abs(ImGui::GetIO().MouseDelta.x);

        // Under the reference speed the multiplier drops below 1 and the drag gets finer than the base;
        // over it, the exponent makes it climb faster than the raw pixel count already does.
        constexpr float ReferencePixels = 5.0f;
        constexpr float Exponent        = 1.5f;
        constexpr float MinScale        = 0.05f;   // ~20x finer than the base at a crawl
        constexpr float MaxScale        = 25.0f;   // capped so a flick cannot fling the value to infinity

        return Math::Clamp(Math::Pow(Pixels / ReferencePixels, Exponent), MinScale, MaxScale);
    }

    /**
     * The property's authored Delta, or the supplied fallback. NOT cursor-scaled.
     *
     * This is the step for the +/- buttons on a NoDrag field, where the user is clicking rather than
     * dragging -- there is no cursor speed to read, and letting one leak in would make a button press
     * step by an arbitrary amount depending on how the mouse happened to be moving.
     */
    inline float ResolveBaseStep(const FProperty* Prop, float FallbackSpeed)
    {
        if (Prop != nullptr && Prop->HasMetadata("Delta"))
        {
            return std::stof(Prop->GetMetadata("Delta").c_str());
        }
        return FallbackSpeed;
    }

    // Step for a click-drag: the base, scaled by cursor speed unless the property authored a Delta.
    // Single place so every numeric widget agrees on the rule.
    inline float ResolveDragSpeed(const FProperty* Prop, float FallbackSpeed)
    {
        if (Prop != nullptr && Prop->HasMetadata("Delta"))
        {
            return std::stof(Prop->GetMetadata("Delta").c_str());
        }
        return FallbackSpeed * MouseAdaptiveDragScale();
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
            float Speed = ResolveDragSpeed(Prop, IsFloatType(DT) ? 0.01f : 1.0f);
            
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
                // Type-only entry; +/- buttons step by Delta, no click-drag scrubbing. The UNSCALED base:
                // a button press must step by the same amount every time.
                ValueType Step = static_cast<ValueType>(ResolveBaseStep(Prop, IsFloatType(DT) ? 0.01f : 1.0f));
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

        EPropertyChangeOp DrawNameCombo(const char* StrId, const TVector<FName>& Choices, const char* ItemIcon,
                                        const char* StaleHint, const char* EmptyHint, bool bAllowCreate);
        EPropertyChangeOp DrawParameterCombo(CAnimationGraph* Graph, bool bObjectValued);
        EPropertyChangeOp DrawCurveCombo(CAnimationGraph* Graph);

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
