#include "PropertyTable.h"

#include <cstdlib>

#include "Containers/HashTable.h"
#include "Containers/Optional.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Reflection/Type/Properties/SubStructProperty.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Customizations/CoreTypeCustomization.h"
#include "Customizations/EntityPropertyCustomization.h"
#include "Memory/Memory.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "Tools/UI/DevelopmentToolUI.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina::PropertyTableInternal
{
    using FRowExpansionState = THashMap<FString, bool>;

    // EditCondition metadata, "Prop", "!Prop", "Prop == V" or "Prop != V" joined by && or ||.
    class FPropertyEditCondition
    {
    public:

        enum class EOperator : uint8
        {
            Truthy,
            Equals,
            NotEquals,
        };

        struct FTerm
        {
            FProperty*  Property = nullptr;
            int64       Expected = 0;
            EOperator   Operator = EOperator::Truthy;
            bool        bNegated = false;
            bool        bLiteralResolved = false;
        };

        void Parse(FProperty* Source);
        bool Evaluate(void* ContainerPtr) const;
        bool IsBound() const { return !Terms.empty(); }
        bool ShouldHide() const { return bHides; }

    private:

        TVector<FTerm>  Terms;
        bool            bRequireAll = true;
        bool            bHides = false;
    };

    class FPropertyRow
    {
    public:

        explicit FPropertyRow(const FPropertyChangedEventCallbacks& InCallbacks);
        FPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);
        virtual ~FPropertyRow() = default;

        FPropertyRow(const FPropertyRow&) = delete;
        FPropertyRow& operator=(const FPropertyRow&) = delete;

        virtual void Update() {}
        virtual void SyncChildren() {}
        virtual void DrawHeader(const FPropertyDrawArgs& Args) {}
        virtual void DrawEditor(const FPropertyDrawArgs& Args) {}
        virtual void DrawChildren(float ChildOffset, const FPropertyDrawArgs& Args);
        virtual float GetMeasuredHeaderTextWidth() const { return 0.0f; }
        virtual bool HasExtraControls() const;
        virtual float GetExtraControlsSectionWidth() const;
        virtual void DrawExtraControlsSection();
        virtual void OnValueResetToDefault() {}
        virtual void SetExpandedRecursive(bool bExpand);
        virtual FString GetExpansionKey() const;
        virtual FStringView GetFilterLabel() const;
        virtual bool IsCategory() const { return false; }

        void UpdateRow();
        void DrawRow(float Offset, const FPropertyDrawArgs& Args);
        float ComputeRequiredHeaderWidth(float Offset) const;
        bool ApplyFilter(const ImGuiTextFilter& Filter, bool bFilterActive, bool bJustActivated, bool bJustCleared, bool bAncestorMatched = false);
        void CaptureExpansion(FRowExpansionState& Out, const FString& Path = FString()) const;
        void RestoreExpansion(const FRowExpansionState& In, const FString& Path = FString());
        void AddPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle);
        FPropertyRow* FindOrCreateCategoryChild(const FName& InCategory);

        const TSharedPtr<FPropertyHandle>& GetPropertyHandle() const { return PropertyHandle; }
        bool HasCustomization() const { return Customization != nullptr; }
        bool PassesFilter() const { return bPassesFilter; }
        bool IsArrayElement() const { return bArrayElement; }
        void SetIsArrayElement(bool bTrue) { bArrayElement = bTrue; }
        bool IsReadOnly() const;
        bool IsVisible() const;
        bool IsTopLevel() const;

    protected:

        void DispatchChange(EPropertyChangeOp Op);
        void PerformResetToDefault();
        void CopyPropertyValue();
        void PastePropertyValue();
        void DestroyChildren();
        void* GetEditConditionContainer() const;
        void BuildMemberRows(CStruct* Type, void* Instance, void* DefaultInstance);

        template<typename TRebuild>
        void RebuildKeepingExpansion(TRebuild&& Rebuild)
        {
            FRowExpansionState State;
            CaptureExpansion(State);
            Rebuild();
            RestoreExpansion(State);
        }

        const FPropertyChangedEventCallbacks&   Callbacks;
        FPropertyEditCondition                  EditCondition;
        TSharedPtr<IPropertyTypeCustomization>  Customization;
        TSharedPtr<FPropertyHandle>             PropertyHandle;
        FPropertyRow*                           ParentRow = nullptr;
        TVector<TUniquePtr<FPropertyRow>>       Children;
        EPropertyChangeOp                       ChangeOp = EPropertyChangeOp::None;
        float                                   LastDrawnHeight = 0.0f;
        bool                                    bEditSessionActive = false;
        bool                                    bArrayElement = false;
        bool                                    bExpanded = true;
        bool                                    bExpandedSaved = true;
        bool                                    bPassesFilter = true;
        bool                                    bDrawReadOnly = false;

    private:

        struct FRowDrawState
        {
            bool bHasProperty = false;
            bool bTopLevel = false;
            bool bMultipleValues = false;
            bool bValuesLocked = false;
            bool bEditable = false;
            bool bDiffers = false;
        };

        void DrawCells(float Offset, const FPropertyDrawArgs& Args, FRowDrawState& State);
        void DrawNameCellActions(const FRowDrawState& State);
        void DrawValueCell(const FPropertyDrawArgs& Args, const FRowDrawState& State);
    };

    class FLeafPropertyRow : public FPropertyRow
    {
    public:

        FLeafPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);

        void Update() override;
        void DrawHeader(const FPropertyDrawArgs& Args) override;
        void DrawEditor(const FPropertyDrawArgs& Args) override;
        float GetMeasuredHeaderTextWidth() const override;
    };

    class FArrayPropertyRow : public FPropertyRow
    {
    public:

        FArrayPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);

        void Update() override;
        void SyncChildren() override;
        void DrawHeader(const FPropertyDrawArgs& Args) override;
        void DrawEditor(const FPropertyDrawArgs& Args) override;
        void DrawChildren(float ChildOffset, const FPropertyDrawArgs& Args) override;
        float GetMeasuredHeaderTextWidth() const override;
        bool HasExtraControls() const override;
        float GetExtraControlsSectionWidth() const override;
        void DrawExtraControlsSection() override;
        void OnValueResetToDefault() override { RebuildChildren(); }
        void SetExpandedRecursive(bool bExpand) override;

        int32 GetElementCount() const;
        bool AllowResize() const;
        bool AllowReorder() const;
        void QueueMoveElement(int64 Index, int64 Target);
        void QueueInsertElement(int64 Index);
        void QueueRemoveElement(int64 Index);

    private:

        void QueueMutation(TFunction<void()> Mutation);
        void RebuildChildren();
        void CaptureElementStates();
        bool IsInnerFixedHeight() const;
        TUniquePtr<FPropertyRow> CreateElementRow(int32 Index);
        void SetWindow(int32 Begin, int32 End);
        void CoverElements(int32 Begin, int32 End);
        void DrawTruncationRow(float Offset, int32 HiddenCount);

        FArrayProperty*                 ArrayProperty = nullptr;
        TVector<TFunction<void()>>      PendingMutations;
        TVector<FRowExpansionState>     ElementStates;
        TOptional<bool>                 ElementExpansion;
        void*                           BuiltArray = nullptr;
        float                           ElementRowHeight = -1.0f;
        int32                           WindowBegin = 0;
        int32                           LastVisibleBegin = 0;
        int32                           LastVisibleEnd = 0;
        bool                            bShowAllElements = false;
    };

    class FMapPropertyRow : public FPropertyRow
    {
    public:

        FMapPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);

        void Update() override;
        void SyncChildren() override;
        void DrawHeader(const FPropertyDrawArgs& Args) override;
        void DrawEditor(const FPropertyDrawArgs& Args) override;
        float GetMeasuredHeaderTextWidth() const override;
        bool HasExtraControls() const override;
        float GetExtraControlsSectionWidth() const override;
        void DrawExtraControlsSection() override;
        void OnValueResetToDefault() override { RebuildChildren(); }

        FMapProperty* GetMapProperty() const { return MapProperty; }
        bool AllowResize() const;
        void QueueRemoveEntry(int64 Index);

    private:

        void QueueMutation(TFunction<void()> Mutation);
        void RebuildChildren();

        FMapProperty*               MapProperty = nullptr;
        TVector<TFunction<void()>>  PendingMutations;
        void*                       BuiltMap = nullptr;
    };

    // A heap key built through the key property, so it never depends on a map instance staying alive.
    class FMapKeyScratch
    {
    public:

        explicit FMapKeyScratch(FMapProperty* Map);
        ~FMapKeyScratch();

        FMapKeyScratch(const FMapKeyScratch&) = delete;
        FMapKeyScratch& operator=(const FMapKeyScratch&) = delete;

        void* Get() const { return Memory; }

    private:

        FProperty*  KeyProperty = nullptr;
        void*       Memory = nullptr;
    };

    // One map entry, its key edited in the header column and its value through a composed row.
    class FMapEntryRow : public FPropertyRow
    {
    public:

        FMapEntryRow(FMapPropertyRow* InMapRow, int64 InIndex, const FPropertyChangedEventCallbacks& InCallbacks);

        void Update() override;
        void DrawHeader(const FPropertyDrawArgs& Args) override;
        void DrawEditor(const FPropertyDrawArgs& Args) override;
        float GetMeasuredHeaderTextWidth() const override;
        bool HasExtraControls() const override;
        float GetExtraControlsSectionWidth() const override;
        void DrawExtraControlsSection() override;
        FStringView GetFilterLabel() const override { return FStringView(); }

    private:

        void CaptureKeySnapshot();
        bool KeyDuplicatesAnotherEntry() const;

        FMapPropertyRow*                        MapRow = nullptr;
        FMapProperty*                           Map = nullptr;
        FProperty*                              KeyProperty = nullptr;
        void*                                   MapContainer = nullptr;
        FMapKeyScratch                          PreviousKey;
        TSharedPtr<FPropertyHandle>             KeyHandle;
        TSharedPtr<IPropertyTypeCustomization>  KeyCustomization;
        TUniquePtr<FPropertyRow>                ValueRow;
        int64                                   EntryIndex = 0;
        EPropertyChangeOp                       KeyChangeOp = EPropertyChangeOp::None;
        bool                                    bKeyEditSessionActive = false;
    };

    class FStructPropertyRow : public FPropertyRow
    {
    public:

        FStructPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);

        void Update() override;
        void SyncChildren() override;
        void DrawHeader(const FPropertyDrawArgs& Args) override;
        void DrawEditor(const FPropertyDrawArgs& Args) override;
        float GetMeasuredHeaderTextWidth() const override;
        void OnValueResetToDefault() override { RebuildChildren(); }

    private:

        void RebuildChildren();
        FFixedString GetHeaderLabel() const;

        FStructProperty*    StructProperty = nullptr;
        void*               BuiltValue = nullptr;
    };

    class FInstancedStructPropertyRow : public FPropertyRow
    {
    public:

        FInstancedStructPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);

        void Update() override;
        void SyncChildren() override;
        void DrawHeader(const FPropertyDrawArgs& Args) override;
        void DrawEditor(const FPropertyDrawArgs& Args) override;
        float GetMeasuredHeaderTextWidth() const override;
        void OnValueResetToDefault() override { RebuildChildren(); }

    private:

        void RebuildChildren();

        FInstancedStructProperty*   InstancedStructProperty = nullptr;
        CStruct*                    CachedType = nullptr;
        void*                       BuiltMemory = nullptr;
    };

    class FOptionalPropertyRow : public FPropertyRow
    {
    public:

        FOptionalPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);

        void Update() override;
        void SyncChildren() override;
        void DrawHeader(const FPropertyDrawArgs& Args) override;
        void DrawEditor(const FPropertyDrawArgs& Args) override;
        float GetMeasuredHeaderTextWidth() const override;
        void OnValueResetToDefault() override { RebuildChildren(); }

    private:

        void RebuildChildren();

        FOptionalProperty*  OptionalProperty = nullptr;
        void*               BuiltPayload = nullptr;
    };

    class FCategoryPropertyRow : public FPropertyRow
    {
    public:

        FCategoryPropertyRow(const FName& InCategory, const FPropertyChangedEventCallbacks& InCallbacks);

        void DrawHeader(const FPropertyDrawArgs& Args) override;
        float GetMeasuredHeaderTextWidth() const override;
        FString GetExpansionKey() const override { return Category.ToString(); }
        FStringView GetFilterLabel() const override;
        bool IsCategory() const override { return true; }

        const FName& GetCategoryName() const { return Category; }

    private:

        FName Category;
    };

    constexpr int32     ComplexArrayDisplayLimit = 32;
    constexpr int32     MaxLiveElementRows = 1024;
    constexpr int32     ElementRowMargin = 64;
    constexpr float     ChildIndentStep = 14.0f;
    constexpr float     ResetColumnWidth = 22.0f;
    constexpr float     ElementButtonWidth = 18.0f;
    constexpr float     MaxHeaderColumnShare = 0.45f;
    constexpr uint32    ArrayControlSeed = 428768833;

    ImU32 ModifiedMarkerColor() { return EditorColors::U32(EditorColors::Warning()); }

    // Byte offset of an edited value inside its outer instance; -1 when it sits in a separate heap block.
    int64 ValueOffsetInOuter(const void* ValuePtr, const void* OuterInstance, const CStruct* OuterType)
    {
        if (ValuePtr == nullptr || OuterInstance == nullptr || OuterType == nullptr)
        {
            return -1;
        }

        const int64 Offset = static_cast<const uint8*>(ValuePtr) - static_cast<const uint8*>(OuterInstance);
        return (Offset >= 0 && Offset < (int64)OuterType->GetSize()) ? Offset : -1;
    }

    // A lone Updated or Finished, from a widget with no drag, is a whole edit that opens and closes its own transaction.
    template<typename TApply>
    void DispatchPropertyChange(EPropertyChangeOp Op, bool& bSessionActive, const FPropertyChangedEventCallbacks& Callbacks, FProperty* Property, const void* ValuePtr, TApply&& Apply)
    {
        if (Op == EPropertyChangeOp::None)
        {
            return;
        }

        const bool bAtomic = !bSessionActive && (Op == EPropertyChangeOp::Updated || Op == EPropertyChangeOp::Finished);
        const bool bStart = Op == EPropertyChangeOp::Started || bAtomic;
        const bool bCommit = Op == EPropertyChangeOp::Finished || bAtomic;
        const FPropertyChangedEvent Event{ Callbacks.Type, Property, Property != nullptr ? Property->Name : FName(),
            bCommit, ValueOffsetInOuter(ValuePtr, Callbacks.Instance, Callbacks.Type) };

        if (bStart && Callbacks.StartChangeCallback)
        {
            Callbacks.StartChangeCallback(Event);
        }
        bSessionActive = Op == EPropertyChangeOp::Started || (bSessionActive && Op != EPropertyChangeOp::Finished);

        FStructOps* Ops = Callbacks.Type != nullptr && Callbacks.Instance != nullptr ? Callbacks.Type->GetStructOps() : nullptr;
        if (bStart && Ops != nullptr && Ops->HasPreEdit())
        {
            Ops->PreEdit(Callbacks.Instance, Event);
        }
        if (Callbacks.PreChangeCallback)
        {
            Callbacks.PreChangeCallback(Event);
        }

        Apply();

        if (bCommit && Ops != nullptr && Ops->HasPostEdit())
        {
            Ops->PostEdit(Callbacks.Instance, Event);
        }
        if (Callbacks.PostChangeCallback)
        {
            Callbacks.PostChangeCallback(Event);
        }
        if (bCommit && Callbacks.FinishChangeCallback)
        {
            Callbacks.FinishChangeCallback(Event);
        }
    }

    ImU32 CategoryBgColor() { return EditorColors::U32(EditorColors::RowBg()); }

    // Caller passes explicit Y bounds; the table row's bottom isn't queryable until after row finalization.
    void DrawModifiedMarker(float RowTopY, float RowBottomY)
    {
        ImGuiTable* Table = ImGui::GetCurrentTable();
        if (Table == nullptr)
        {
            return;
        }
        const float X = Table->Columns[0].MinX;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(X, RowTopY), ImVec2(X + 2.5f, RowBottomY), ModifiedMarkerColor());
    }

    bool IsFixedHeightPropertyType(EPropertyTypeFlags Type)
    {
        switch (Type)
        {
        case EPropertyTypeFlags::Struct:
        case EPropertyTypeFlags::Vector:
        case EPropertyTypeFlags::Map:
        case EPropertyTypeFlags::Optional:
        case EPropertyTypeFlags::InstancedStruct:
            return false;
        default:
            return true;
        }
    }

    TUniquePtr<FPropertyRow> CreatePropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
    {
        switch (InPropHandle->Property->GetType())
        {
        case EPropertyTypeFlags::Vector:
            return MakeUnique<FArrayPropertyRow>(InPropHandle, InParentRow, InCallbacks);
        case EPropertyTypeFlags::Map:
            return MakeUnique<FMapPropertyRow>(InPropHandle, InParentRow, InCallbacks);
        case EPropertyTypeFlags::Struct:
            return MakeUnique<FStructPropertyRow>(InPropHandle, InParentRow, InCallbacks);
        case EPropertyTypeFlags::Optional:
            return MakeUnique<FOptionalPropertyRow>(InPropHandle, InParentRow, InCallbacks);
        case EPropertyTypeFlags::InstancedStruct:
            return MakeUnique<FInstancedStructPropertyRow>(InPropHandle, InParentRow, InCallbacks);
        default:
            return MakeUnique<FLeafPropertyRow>(InPropHandle, InParentRow, InCallbacks);
        }
    }

    // "Outer|Inner" as one name per nesting level, skipping empty segments.
    TVector<FName> SplitCategoryPath(FStringView Path)
    {
        TVector<FName> Segments;
        size_t Start = 0;
        while (Start <= Path.size())
        {
            size_t Sep = Path.find('|', Start);
            if (Sep == FStringView::npos)
            {
                Sep = Path.size();
            }
            if (Sep > Start)
            {
                // Through FStringView, since FName(const char*, uint32) is a base plus a number, not a length.
                Segments.push_back(FName(Path.substr(Start, Sep - Start)));
            }
            Start = Sep + 1;
        }
        return Segments;
    }

    FStringView TrimWhitespace(FStringView In)
    {
        while (!In.empty() && (In.front() == ' ' || In.front() == '\t'))
        {
            In.remove_prefix(1);
        }
        while (!In.empty() && (In.back() == ' ' || In.back() == '\t'))
        {
            In.remove_suffix(1);
        }
        return In;
    }

    bool IsIntegerPropertyType(EPropertyTypeFlags Type)
    {
        switch (Type)
        {
        case EPropertyTypeFlags::Int8:
        case EPropertyTypeFlags::Int16:
        case EPropertyTypeFlags::Int32:
        case EPropertyTypeFlags::Int64:
        case EPropertyTypeFlags::UInt8:
        case EPropertyTypeFlags::UInt16:
        case EPropertyTypeFlags::UInt32:
        case EPropertyTypeFlags::UInt64:
        case EPropertyTypeFlags::Bool:
            return true;
        default:
            return false;
        }
    }

    bool TryReadConditionInteger(FProperty* Property, void* ContainerPtr, int64& OutValue)
    {
        void* ValuePtr = Property->GetValuePtr<uint8>(ContainerPtr);
        if (ValuePtr == nullptr)
        {
            return false;
        }

        const EPropertyTypeFlags Type = Property->GetType();
        if (Type == EPropertyTypeFlags::Enum)
        {
            FNumericProperty* Inner = static_cast<FEnumProperty*>(Property)->GetInnerProperty();
            if (Inner == nullptr)
            {
                return false;
            }
            OutValue = Inner->GetSignedIntPropertyValue(ValuePtr);
            return true;
        }

        if (IsIntegerPropertyType(Type))
        {
            OutValue = static_cast<FNumericProperty*>(Property)->GetSignedIntPropertyValue(ValuePtr);
            return true;
        }

        return false;
    }

    // The literal side of an == / != term, as an integer. Enum terms accept a bare or qualified enumerator.
    bool TryResolveConditionLiteral(FProperty* Property, FStringView Literal, int64& OutValue)
    {
        if (Property->GetType() == EPropertyTypeFlags::Enum)
        {
            CEnum* Enum = static_cast<FEnumProperty*>(Property)->GetEnum();
            if (Enum == nullptr)
            {
                return false;
            }

            for (const TPair<FName, uint64>& Entry : Enum->Names)
            {
                const FStringView Name(Entry.first.c_str());
                const size_t Scope = Name.rfind("::");
                const bool bMatches = Name == Literal
                    || (Scope != FStringView::npos && Name.substr(Scope + 2) == Literal);
                if (bMatches)
                {
                    OutValue = static_cast<int64>(Entry.second);
                    return true;
                }
            }
            return false;
        }

        if (Literal == "true")
        {
            OutValue = 1;
            return true;
        }
        if (Literal == "false")
        {
            OutValue = 0;
            return true;
        }

        const FString Text(Literal.data(), Literal.size());
        char* End = nullptr;
        const long long Parsed = strtoll(Text.c_str(), &End, 10);
        if (End == Text.c_str())
        {
            return false;
        }
        OutValue = static_cast<int64>(Parsed);
        return true;
    }

    bool EvaluateConditionTerm(const FPropertyEditCondition::FTerm& Term, void* ContainerPtr)
    {
        bool bResult = false;

        if (Term.Operator == FPropertyEditCondition::EOperator::Truthy)
        {
            if (Term.Property->GetType() == EPropertyTypeFlags::Object)
            {
                TStrongObjectPtr<CObject> Value;
                Term.Property->GetValue(ContainerPtr, &Value);
                bResult = Value != nullptr;
            }
            else
            {
                int64 Value = 0;
                bResult = TryReadConditionInteger(Term.Property, ContainerPtr, Value) && Value != 0;
            }
        }
        else
        {
            int64 Value = 0;
            if (Term.bLiteralResolved && TryReadConditionInteger(Term.Property, ContainerPtr, Value))
            {
                bResult = Term.Operator == FPropertyEditCondition::EOperator::Equals ? Value == Term.Expected : Value != Term.Expected;
            }
        }

        return Term.bNegated ? !bResult : bResult;
    }

    // One buffer for the whole editor, so a value can move between panels, not just within one table.
    TVector<uint8> GPropertyClipboardBytes;
    FProperty*     GPropertyClipboardSource = nullptr;

    // Two different structs agree on flags and size, so a paste between them reinterprets raw bytes.
    bool ArePropertiesCompatible(FProperty* A, FProperty* B)
    {
        if (A == nullptr || B == nullptr)
        {
            return false;
        }

        if (A->GetType() != B->GetType() || A->GetElementSize() != B->GetElementSize())
        {
            return false;
        }

        if (A->IsA(EPropertyTypeFlags::Vector) || A->IsA(EPropertyTypeFlags::Map))
        {
            return false;
        }

        if (A->IsA(EPropertyTypeFlags::Struct))
        {
            return static_cast<FStructProperty*>(A)->GetStruct() == static_cast<FStructProperty*>(B)->GetStruct();
        }

        if (A->IsA(EPropertyTypeFlags::Object))
        {
            return static_cast<FObjectProperty*>(A)->GetPropertyClass() == static_cast<FObjectProperty*>(B)->GetPropertyClass();
        }

        if (A->IsA(EPropertyTypeFlags::SoftObject))
        {
            return static_cast<FSoftObjectProperty*>(A)->GetPropertyClass() == static_cast<FSoftObjectProperty*>(B)->GetPropertyClass();
        }

        if (A->IsA(EPropertyTypeFlags::Class))
        {
            return static_cast<FClassProperty*>(A)->GetMetaClass() == static_cast<FClassProperty*>(B)->GetMetaClass();
        }

        if (A->IsA(EPropertyTypeFlags::SubStruct))
        {
            return static_cast<FSubStructProperty*>(A)->GetMetaStruct() == static_cast<FSubStructProperty*>(B)->GetMetaStruct();
        }

        if (A->IsA(EPropertyTypeFlags::Enum))
        {
            return static_cast<FEnumProperty*>(A)->GetEnum() == static_cast<FEnumProperty*>(B)->GetEnum();
        }

        if (A->IsA(EPropertyTypeFlags::Optional))
        {
            return ArePropertiesCompatible(static_cast<FOptionalProperty*>(A)->GetInternalProperty(), static_cast<FOptionalProperty*>(B)->GetInternalProperty());
        }

        return true;
    }

    bool IsCopyableProperty(FProperty* Property)
    {
        return ArePropertiesCompatible(Property, Property);
    }

    void ClearPropertyClipboard()
    {
        GPropertyClipboardBytes.clear();
        GPropertyClipboardSource = nullptr;
    }

    // Rows are drawn whole while anything in the window is being edited or a keyboard move is looking for its target.
    bool IsRowOutsideView(const ImGuiTable& Table, float Height)
    {
        if (FPropertyTable::IsDetailsRemeasureFrame())
        {
            return false;
        }
        const ImGuiContext& Context = *GImGui;
        if (Context.NavMoveScoringItems || (Context.ActiveId != 0 && Context.ActiveIdWindow == Context.CurrentWindow))
        {
            return false;
        }
        return Table.RowPosY1 > Table.InnerClipRect.Max.y || Table.RowPosY1 + Height < Table.InnerClipRect.Min.y;
    }

    // A clipped label leads its tooltip with the full name, since the cell no longer shows it.
    void DrawPropertyTooltip(const FProperty* Property, bool bLabelClipped = false)
    {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            return;
        }

        const FCStringView Name = Property->GetPropertyDisplayName();
        const FStringView Tooltip = Property->GetMetadata("ToolTip");
        if (Tooltip.empty())
        {
            ImGuiX::WrappedTooltip_Internal(FStringView(Name.c_str(), Name.size()));
            return;
        }
        if (bLabelClipped)
        {
            const FString Both = FString(Name.c_str()) + "\n\n" + FString(Tooltip.data(), Tooltip.size());
            ImGuiX::WrappedTooltip_Internal(FStringView(Both.c_str(), Both.size()));
            return;
        }
        ImGuiX::WrappedTooltip_Internal(Tooltip);
    }

    // An element's inner property has no name or tooltip, so the element reads as its place in the array plus the array's own tooltip.
    void DrawRowTooltip(const FPropertyHandle* Handle, const FProperty* Property, bool bLabelClipped, FStringView Label = FStringView())
    {
        if (Handle == nullptr || Handle->OwnerArray == nullptr)
        {
            DrawPropertyTooltip(Property, bLabelClipped);
            return;
        }
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            return;
        }

        const FArrayProperty* Array = Handle->OwnerArray;
        const FString TypeName = Property->GetType() == EPropertyTypeFlags::Struct
            ? static_cast<const FStructProperty*>(Property)->GetStruct()->GetName().ToString()
            : FString(PropertyTypePlainNames[(size_t)Property->GetType()]);

        FString Text;
        if (bLabelClipped && !Label.empty())
        {
            Text += FString(Label.data(), Label.size()) + "\n\n";
        }
        Text += Lumina::Format("Element {} of {} ({})", Handle->Index, Array->GetPropertyDisplayName().c_str(), TypeName.c_str());

        const FStringView ArrayTooltip = Array->GetMetadata("ToolTip");
        if (!ArrayTooltip.empty())
        {
            Text += "\n\n" + FString(ArrayTooltip.data(), ArrayTooltip.size());
        }
        ImGuiX::WrappedTooltip_Internal(FStringView(Text.c_str(), Text.size()));
    }

    // A label too long for its cell ends in an ellipsis rather than being cut mid-letter, and reports that it did.
    bool DrawHeaderLabel(const char* Label)
    {
        const bool bClipped = ImGui::CalcTextSize(Label).x > ImGui::GetContentRegionAvail().x;
        ImGui::TextAligned(0.0f, -FLT_MIN, "%s", Label);
        return bClipped;
    }

    // The foldout version of DrawHeaderLabel, drawing the text itself over a header whose own label is hidden.
    bool CollapsingHeaderEllipsis(const char* Label, ImGuiTreeNodeFlags Flags, bool& bOutClipped, const void* StableId = nullptr)
    {
        if (StableId != nullptr)
        {
            ImGui::PushID(StableId);
        }
        else
        {
            ImGui::PushID(Label);
        }
        const bool bOpen = ImGui::CollapsingHeader("##Header", Flags);
        ImGui::PopID();

        const ImGuiStyle& Style = ImGui::GetStyle();
        const ImVec2 Min = ImGui::GetItemRectMin();
        const ImVec2 Max = ImGui::GetItemRectMax();
        const bool bLeaf = (Flags & ImGuiTreeNodeFlags_Leaf) != 0;
        const float ArrowWidth = bLeaf ? Style.FramePadding.x : ImGui::GetFontSize() + Style.FramePadding.x * 3.0f;
        const ImVec2 TextSize = ImGui::CalcTextSize(Label);
        const ImVec2 TextMin(Min.x + ArrowWidth, Min.y + Style.FramePadding.y);
        const ImVec2 TextMax(Max.x - Style.FramePadding.x, Max.y);

        bOutClipped = TextMin.x + TextSize.x > TextMax.x;
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), TextMin, TextMax, TextMax.x, Label, nullptr, &TextSize);
        return bOpen;
    }

    // A transparent foldout whose open state the row keeps, rather than ImGui's storage.
    bool DrawRowFoldout(const char* Label, bool bOpen, ImGuiTreeNodeFlags Flags, bool& bOutClipped, const void* StableId = nullptr)
    {
        ImGui::SetNextItemOpen(bOpen);
        ImGui::PushStyleColor(ImGuiCol_Header, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
        const bool bResult = CollapsingHeaderEllipsis(Label, Flags, bOutClipped, StableId);
        ImGui::PopStyleColor(3);
        return bResult;
    }

    // Shared by the leaf row and the map-entry editors, returning null for struct and array types.
    TSharedPtr<IPropertyTypeCustomization> MakeLeafPropertyCustomization(FProperty* Prop)
    {
        switch (Prop->GetType())
        {
        case EPropertyTypeFlags::Int8:   return FNumericPropertyCustomization<int8,   ImGuiDataType_S8>::MakeInstance();
        case EPropertyTypeFlags::Int16:  return FNumericPropertyCustomization<int16,  ImGuiDataType_S16>::MakeInstance();
        case EPropertyTypeFlags::Int32:  return FNumericPropertyCustomization<int32,  ImGuiDataType_S32>::MakeInstance();
        case EPropertyTypeFlags::Int64:  return FNumericPropertyCustomization<int64,  ImGuiDataType_S64>::MakeInstance();
        case EPropertyTypeFlags::UInt8:  return FNumericPropertyCustomization<uint8,  ImGuiDataType_U8>::MakeInstance();
        case EPropertyTypeFlags::UInt16: return FNumericPropertyCustomization<uint16, ImGuiDataType_U16>::MakeInstance();
        case EPropertyTypeFlags::UInt32: return FNumericPropertyCustomization<uint32, ImGuiDataType_U32>::MakeInstance();
        case EPropertyTypeFlags::Entity: return FEntityPropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::UInt64: return FNumericPropertyCustomization<uint64, ImGuiDataType_U64>::MakeInstance();
        case EPropertyTypeFlags::Float:  return FNumericPropertyCustomization<float,  ImGuiDataType_Float>::MakeInstance();
        case EPropertyTypeFlags::Double: return FNumericPropertyCustomization<double, ImGuiDataType_Double>::MakeInstance();
        case EPropertyTypeFlags::Bool:   return FBoolPropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::Object: return FCObjectPropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::SoftObject: return FSoftObjectPropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::Class:  return FClassPropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::SubStruct: return FSubStructPropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::Name:   return FNamePropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::String: return FStringPropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::Enum:
        {
            // An enum can register its own drawing by name, the way a struct does.
            FPropertyCustomizationRegistry* Registry = GEngine->GetDevelopmentToolsUI()->GetPropertyCustomizationRegistry();
            TSharedPtr<IPropertyTypeCustomization> Registered = Registry->GetPropertyCustomizationForType(static_cast<FEnumProperty*>(Prop)->GetEnum()->GetName());
            return Registered ? Registered : FEnumPropertyCustomization::MakeInstance();
        }
        case EPropertyTypeFlags::Delegate: return FDelegatePropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::ComponentRef: return FComponentRefPropertyCustomization::MakeInstance();
        default: return nullptr;
        }
    }

    // Disabled rather than hidden when it cannot act, so every element row keeps the same columns.
    bool ElementButton(const char* Icon, bool bEnabled, const char* Tooltip)
    {
        ImGui::BeginDisabled(!bEnabled);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
        const bool bClicked = ImGuiX::FlatButton(Icon, ImVec2(ElementButtonWidth, 24), ArrayControlSeed);
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGuiX::TextTooltip_Internal(Tooltip);
        }
        return bClicked;
    }

    float ContainerControlsWidth()
    {
        return ElementButtonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    }

    TSharedPtr<FPropertyHandle> MakeMapValueHandle(FMapPropertyRow* MapRow, int64 Index)
    {
        FMapProperty* Map = MapRow->GetMapProperty();
        void* Container = MapRow->GetPropertyHandle()->GetValuePtr();
        void* DefaultContainer = MapRow->GetPropertyHandle()->GetDefaultValuePtr();
        return MakeShared<FPropertyHandle>(Map, Container, DefaultContainer, Map->GetValueProperty(), Index, false);
    }

    const char* InstancedStructLabel(const CStruct* Type)
    {
        return StructIdentity(Type).c_str();
    }

    // Whether Candidate lists Interface among the interfaces its C# type implements.
    bool ImplementsInterface(const CStruct* Candidate, FStringView Interface)
    {
        if (!Candidate->HasMeta("Implements"))
        {
            return false;
        }
        const FString& Listed = Candidate->GetMeta("Implements");
        const FStringView All(Listed.c_str(), Listed.size());
        size_t Start = 0;
        while (Start <= All.size())
        {
            size_t End = Start;
            while (End < All.size() && All[End] != ';')
            {
                ++End;
            }
            if (All.substr(Start, End - Start) == Interface)
            {
                return true;
            }
            Start = End + 1;
        }
        return false;
    }

    // A None entry sits at index 0. A nonempty Implements keeps only the structs that implement that interface.
    CStruct* DrawInstancedStructTypePicker(const char* StrId, CStruct* Base, FStringView Implements, CStruct* Current, bool& bOutChanged)
    {
        bOutChanged = false;

        // Gathered only while the list is open, since walking every struct each frame for a closed combo is wasted.
        TVector<CStruct*> Candidates;
        if (ImGuiX::IsSearchableComboOpen(StrId))
        {
            for (TObjectIterator<CStruct> It; It; ++It)
            {
                CStruct* Candidate = *It;
                if (IsInstancableStructType(Candidate) && (Base == nullptr || Candidate->IsChildOf(Base))
                    && (Implements.empty() || ImplementsInterface(Candidate, Implements)))
                {
                    Candidates.push_back(Candidate);
                }
            }

            Algo::Sort(Candidates, [](CStruct* A, CStruct* B)
            {
                return strcmp(InstancedStructLabel(A), InstancedStructLabel(B)) < 0;
            });
        }

        int32 CurrentIndex = 0;
        for (size_t i = 0; i < Candidates.size(); ++i)
        {
            if (Candidates[i] == Current)
            {
                CurrentIndex = static_cast<int32>(i + 1);
                break;
            }
        }

        const char* Preview = Current ? InstancedStructLabel(Current) : "None";

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        const int32 Picked = ImGuiX::SearchableCombo(StrId, Preview, static_cast<int32>(Candidates.size()) + 1, CurrentIndex,
            [&Candidates](int32 Index) -> FFixedString
            {
                return (Index == 0) ? FFixedString("None") : FFixedString(InstancedStructLabel(Candidates[Index - 1]));
            }, LE_ICON_CUBE_OUTLINE);
        ImGui::PopItemWidth();

        if (Picked != Constants::kIndexNone)
        {
            bOutChanged = true;
            return (Picked == 0) ? nullptr : Candidates[Picked - 1];
        }

        return Current;
    }

    // The type's default instance to diff against, unless the object is that default itself.
    void* DefaultInstanceFor(void* InObject, CStruct* InType)
    {
        void* Default = InType != nullptr && InObject != nullptr ? InType->GetDefaultInstance() : nullptr;
        return Default != InObject ? Default : nullptr;
    }

    void FPropertyEditCondition::Parse(FProperty* Source)
    {
        Terms.clear();
        bRequireAll = true;
        bHides = false;

        const FCStringView Expression = Source->GetMetadata("EditCondition");
        if (Expression.empty())
        {
            return;
        }

        const FStringView Full = Expression;
        TVector<FStringView> TermViews;
        size_t Start = 0;
        while (Start <= Full.size())
        {
            const size_t And = Full.find("&&", Start);
            const size_t Or  = Full.find("||", Start);
            const size_t Sep = And < Or ? And : Or;
            if (Sep == FStringView::npos)
            {
                TermViews.push_back(Full.substr(Start));
                break;
            }
            bRequireAll &= (Sep == And);
            TermViews.push_back(Full.substr(Start, Sep - Start));
            Start = Sep + 2;
        }

        for (FStringView TermView : TermViews)
        {
            FTerm Term;
            FStringView Text = TrimWhitespace(TermView);
            if (!Text.empty() && Text.front() == '!' && (Text.size() < 2 || Text[1] != '='))
            {
                Term.bNegated = true;
                Text.remove_prefix(1);
            }

            FStringView NameView = Text;
            FStringView LiteralView;
            const size_t Equals    = Text.find("==");
            const size_t NotEquals = Text.find("!=");
            if (Equals != FStringView::npos || NotEquals != FStringView::npos)
            {
                const size_t Sep = Equals < NotEquals ? Equals : NotEquals;
                Term.Operator = Sep == Equals ? EOperator::Equals : EOperator::NotEquals;
                NameView = Text.substr(0, Sep);

                LiteralView = TrimWhitespace(Text.substr(Sep + 2));
                if (LiteralView.size() >= 2 && LiteralView.front() == '"' && LiteralView.back() == '"')
                {
                    LiteralView = LiteralView.substr(1, LiteralView.size() - 2);
                }
            }

            NameView = TrimWhitespace(NameView);
            if (NameView.empty())
            {
                Terms.clear();
                return;
            }

            CStruct* Owner = Source->GetOwnerStruct();
            Term.Property = Owner != nullptr ? Owner->GetProperty(FName(NameView)) : nullptr;
            if (Term.Property == nullptr || Term.Property == Source)
            {
                Terms.clear();
                return;
            }

            if (Term.Operator != EOperator::Truthy)
            {
                Term.bLiteralResolved = TryResolveConditionLiteral(Term.Property, LiteralView, Term.Expected);
            }

            Terms.push_back(Term);
        }

        bHides = Source->HasEditorFlag(EPropertyEditorFlags::EditConditionHides);
    }

    bool FPropertyEditCondition::Evaluate(void* ContainerPtr) const
    {
        if (Terms.empty() || ContainerPtr == nullptr)
        {
            return true;
        }

        for (const FTerm& Term : Terms)
        {
            const bool bTermResult = EvaluateConditionTerm(Term, ContainerPtr);
            if (bRequireAll && !bTermResult)
            {
                return false;
            }
            if (!bRequireAll && bTermResult)
            {
                return true;
            }
        }

        return bRequireAll;
    }

    FPropertyRow::FPropertyRow(const FPropertyChangedEventCallbacks& InCallbacks)
        : Callbacks(InCallbacks)
    {
    }

    FPropertyRow::FPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : Callbacks(InCallbacks)
        , PropertyHandle(InPropHandle)
        , ParentRow(InParentRow)
    {
        if (GetEditConditionContainer() != nullptr)
        {
            EditCondition.Parse(PropertyHandle->Property);
        }
    }

    void* FPropertyRow::GetEditConditionContainer() const
    {
        if (PropertyHandle == nullptr || PropertyHandle->Property == nullptr)
        {
            return nullptr;
        }
        if (PropertyHandle->OwnerArray != nullptr || PropertyHandle->OwnerMap != nullptr)
        {
            return nullptr;
        }
        return PropertyHandle->ContainerPtr;
    }

    bool FPropertyRow::IsTopLevel() const
    {
        if (ParentRow == nullptr || !ParentRow->IsCategory())
        {
            return false;
        }
        for (const FPropertyRow* Row = ParentRow; Row != nullptr; Row = Row->ParentRow)
        {
            if (!Row->IsCategory())
            {
                return false;
            }
        }
        return true;
    }

    FPropertyRow* FPropertyRow::FindOrCreateCategoryChild(const FName& InCategory)
    {
        for (const TUniquePtr<FPropertyRow>& Child : Children)
        {
            if (Child->IsCategory() && static_cast<FCategoryPropertyRow*>(Child.get())->GetCategoryName() == InCategory)
            {
                return Child.get();
            }
        }

        TUniquePtr<FCategoryPropertyRow> NewRow = MakeUnique<FCategoryPropertyRow>(InCategory, Callbacks);
        NewRow->ParentRow = this;
        FPropertyRow* Row = NewRow.get();
        Children.push_back(Move(NewRow));
        return Row;
    }

    void FPropertyRow::AddPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle)
    {
        Children.push_back(CreatePropertyRow(InPropHandle, this, Callbacks));
    }

    void FPropertyRow::BuildMemberRows(CStruct* Type, void* Instance, void* DefaultInstance)
    {
        DestroyChildren();
        if (Type == nullptr || Instance == nullptr)
        {
            return;
        }

        bool bSingleCategory = true;
        FCStringView FirstCategory;
        bool bFirst = true;
        for (FProperty* Member : Type->GetProperties())
        {
            if (!Member->IsVisible())
            {
                continue;
            }
            const FCStringView Category = Member->HasMetadata("Category") ? Member->GetMetadata("Category") : FCStringView();
            if (bFirst)
            {
                FirstCategory = Category;
                bFirst = false;
            }
            else if (FStringView(Category.c_str(), Category.size()) != FStringView(FirstCategory.c_str(), FirstCategory.size()))
            {
                bSingleCategory = false;
                break;
            }
        }

        for (FProperty* Member : Type->GetProperties())
        {
            if (!Member->IsVisible())
            {
                continue;
            }

            FPropertyRow* Target = this;
            if (!bSingleCategory && Member->HasMetadata("Category"))
            {
                const FCStringView Path = Member->GetMetadata("Category");
                for (const FName& Segment : SplitCategoryPath(FStringView(Path.c_str(), Path.size())))
                {
                    Target = Target->FindOrCreateCategoryChild(Segment);
                }
            }

            Target->AddPropertyRow(MakeShared<FPropertyHandle>(Instance, DefaultInstance, Member));
        }
    }

    bool FPropertyRow::IsVisible() const
    {
        if (!EditCondition.IsBound() || !EditCondition.ShouldHide())
        {
            return true;
        }
        return EditCondition.Evaluate(GetEditConditionContainer());
    }

    void FPropertyRow::SetExpandedRecursive(bool bExpand)
    {
        // The saved state too, so a filter that is active right now does not undo the choice when it clears.
        bExpanded = bExpand;
        bExpandedSaved = bExpand;
        for (const TUniquePtr<FPropertyRow>& Child : Children)
        {
            Child->SetExpandedRecursive(bExpand);
        }
    }

    FString FPropertyRow::GetExpansionKey() const
    {
        if (PropertyHandle == nullptr || PropertyHandle->Property == nullptr)
        {
            return FString();
        }
        if (IsArrayElement())
        {
            return Lumina::Format("[{}]", PropertyHandle->Index);
        }
        return PropertyHandle->Property->GetPropertyName().ToString();
    }

    void FPropertyRow::CaptureExpansion(FRowExpansionState& Out, const FString& Path) const
    {
        Out[Path] = bExpanded;
        for (const TUniquePtr<FPropertyRow>& Child : Children)
        {
            Child->CaptureExpansion(Out, Path + "/" + Child->GetExpansionKey());
        }
    }

    void FPropertyRow::RestoreExpansion(const FRowExpansionState& In, const FString& Path)
    {
        if (auto Found = In.find(Path); Found != In.end())
        {
            bExpanded = Found->second;
            bExpandedSaved = Found->second;
        }
        for (const TUniquePtr<FPropertyRow>& Child : Children)
        {
            Child->RestoreExpansion(In, Path + "/" + Child->GetExpansionKey());
        }
    }

    void FPropertyRow::DestroyChildren()
    {
        Children.clear();
    }

    void FPropertyRow::UpdateRow()
    {
        SyncChildren();
        Update();

        // Children only accumulate ChangeOp when drawn, and a huge array gets expensive.
        if (bExpanded)
        {
            for (const TUniquePtr<FPropertyRow>& Child : Children)
            {
                Child->UpdateRow();
            }
        }
    }

    float FPropertyRow::ComputeRequiredHeaderWidth(float Offset) const
    {
        float Width = Offset + GetMeasuredHeaderTextWidth();
        if (bExpanded)
        {
            for (const TUniquePtr<FPropertyRow>& Child : Children)
            {
                if (!Child->IsVisible())
                {
                    continue;
                }
                Width = Math::Max(Width, Child->ComputeRequiredHeaderWidth(Offset + ChildIndentStep));
            }
        }
        return Width;
    }

    void FPropertyRow::CopyPropertyValue()
    {
        void* Value = PropertyHandle != nullptr ? PropertyHandle->GetValuePtr() : nullptr;
        if (Value == nullptr || PropertyHandle->Property == nullptr)
        {
            return;
        }

        if (!IsCopyableProperty(PropertyHandle->Property))
        {
            ImGuiX::Notifications::NotifyWarning("'{0}' is not a copyable property type.", PropertyHandle->Property->Name);
            return;
        }

        // The source can be a property of a script type, which a reload frees.
        static const FDelegateHandle ClearOnReload = FCoreDelegates::OnScriptsWillReload.AddLambda([] { ClearPropertyClipboard(); });
        (void)ClearOnReload;

        GPropertyClipboardBytes.clear();

        // The same serialization CopyPropertiesTo uses, so object references survive as GUIDs.
        FMemoryWriter Writer(GPropertyClipboardBytes);
        FObjectProxyArchiver Proxy(Writer, true);
        PropertyHandle->Property->Serialize(Proxy, Value);

        GPropertyClipboardSource = PropertyHandle->Property;
        ImGuiX::Notifications::NotifySuccess("Copied '{0}'.", PropertyHandle->Property->Name);
    }

    void FPropertyRow::PastePropertyValue()
    {
        void* Value = PropertyHandle != nullptr ? PropertyHandle->GetValuePtr() : nullptr;
        if (Value == nullptr || PropertyHandle->Property == nullptr || GPropertyClipboardSource == nullptr)
        {
            return;
        }

        if (!ArePropertiesCompatible(GPropertyClipboardSource, PropertyHandle->Property))
        {
            ImGuiX::Notifications::NotifyWarning("Clipboard holds a '{0}'; cannot paste into '{1}'.",
                GPropertyClipboardSource->Name, PropertyHandle->Property->Name);
            return;
        }

        DispatchChange(EPropertyChangeOp::Started);

        FMemoryReader Reader(GPropertyClipboardBytes);
        FObjectProxyArchiver Proxy(Reader, true);
        PropertyHandle->Property->Serialize(Proxy, Value);
        if (Customization)
        {
            Customization->HandleExternalUpdate(PropertyHandle);
        }

        DispatchChange(EPropertyChangeOp::Finished);

        ImGuiX::Notifications::NotifySuccess("Pasted into '{0}'.", PropertyHandle->Property->Name);
    }

    void FPropertyRow::PerformResetToDefault()
    {
        if (PropertyHandle == nullptr || !PropertyHandle->HasDefault())
        {
            return;
        }

        DispatchChange(EPropertyChangeOp::Started);

        PropertyHandle->ResetToDefault();
        if (Customization)
        {
            Customization->HandleExternalUpdate(PropertyHandle);
        }
        OnValueResetToDefault();

        DispatchChange(EPropertyChangeOp::Finished);
    }

    void FPropertyRow::DispatchChange(EPropertyChangeOp Op)
    {
        if (Op == EPropertyChangeOp::None || PropertyHandle == nullptr || PropertyHandle->Property == nullptr)
        {
            return;
        }

        DispatchPropertyChange(Op, bEditSessionActive, Callbacks, PropertyHandle->Property, PropertyHandle->GetValuePtr(), [this]
        {
            if (Customization)
            {
                Customization->UpdatePropertyValue(PropertyHandle);
            }
        });
    }

    void FPropertyRow::DrawRow(float Offset, const FPropertyDrawArgs& Args)
    {
        // First, since HasExtraControls reads it through AllowResize.
        bDrawReadOnly = Args.bReadOnly;

        FRowDrawState State;
        State.bHasProperty = !IsCategory() && PropertyHandle != nullptr && PropertyHandle->Property != nullptr;
        State.bTopLevel = State.bHasProperty && IsTopLevel();
        State.bMultipleValues = State.bTopLevel && Callbacks.IsMultiValueFn && Callbacks.IsMultiValueFn(PropertyHandle->Property);
        State.bValuesLocked = State.bMultipleValues && !Callbacks.bEditMultipleValues;

        ImGui::PushID(this);
        ImGui::TableNextRow();

        // An offscreen row holds its measured height, so the scroll range and every row below stay where drawing would put them.
        ImGuiTable* Table = ImGui::GetCurrentTable();
        if (Table != nullptr && LastDrawnHeight > 0.0f && IsRowOutsideView(*Table, LastDrawnHeight))
        {
            Table->RowPosY2 = ImMax(Table->RowPosY2, Table->RowPosY1 + LastDrawnHeight);
        }
        else
        {
            DrawCells(Offset, Args, State);
        }

        ImGui::PopID();

        // The aggregate stands in for the subtree, which would otherwise show the focus object's values.
        if (bExpanded && !State.bValuesLocked)
        {
            // Synced here as well as in UpdateRow, since a paste or a hook earlier this frame can move the value.
            SyncChildren();
            if (!Children.empty())
            {
                ImGui::BeginDisabled(IsReadOnly());
                DrawChildren(Offset + ChildIndentStep, Args);
                ImGui::EndDisabled();
            }
        }
    }

    void FPropertyRow::DrawCells(float Offset, const FPropertyDrawArgs& Args, FRowDrawState& State)
    {
        const float RowTopY = ImGui::GetCurrentTable()->RowPosY1;
        State.bEditable = !Args.bReadOnly && !State.bValuesLocked && !IsReadOnly();
        State.bDiffers = State.bHasProperty && PropertyHandle->HasDefault() && PropertyHandle->DiffersFromDefault();

        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();
        DrawHeader(Args);
        DrawNameCellActions(State);

        ImGui::TableNextColumn();
        DrawValueCell(Args, State);

        // Fetched again, since a table first seen inside this row can grow ImGui's table pool and move the one above.
        if (const ImGuiTable* RowTable = ImGui::GetCurrentTable())
        {
            const float RowBottom = ImMax(RowTable->RowPosY2, ImGui::GetCurrentWindow()->DC.CursorMaxPos.y + RowTable->RowCellPaddingY);
            LastDrawnHeight = RowBottom - RowTable->RowPosY1;
        }

        if (State.bDiffers)
        {
            DrawModifiedMarker(RowTopY, ImGui::GetCursorScreenPos().y);
        }
    }

    void FPropertyRow::DrawNameCellActions(const FRowDrawState& State)
    {
        // Hung off the name cell, the row's only stable click target, since the value cell is the widget's.
        const bool bShiftHeld = ImGui::GetIO().KeyShift;
        if (State.bHasProperty && bShiftHeld && ImGui::IsItemHovered())
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                CopyPropertyValue();
            }
            else if (State.bEditable && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                PastePropertyValue();
            }
        }

        // Skipped while Shift is held, so the copy chord does not also open the menu.
        const bool bCanExpand = !Children.empty();
        if (bShiftHeld || (!State.bHasProperty && !bCanExpand) || !ImGui::BeginPopupContextItem("##PropCtx"))
        {
            return;
        }

        if (State.bHasProperty)
        {
            ImGui::BeginDisabled(!State.bDiffers || !State.bEditable);
            if (ImGui::MenuItem(LE_ICON_REFRESH " Reset to Default"))
            {
                PerformResetToDefault();
            }
            ImGui::EndDisabled();
        }
        if (State.bHasProperty && bCanExpand)
        {
            ImGui::Separator();
        }
        if (bCanExpand && ImGui::MenuItem(LE_ICON_EXPAND_ALL " Expand All"))
        {
            SetExpandedRecursive(true);
        }
        if (bCanExpand && ImGui::MenuItem(LE_ICON_COLLAPSE_ALL " Collapse All"))
        {
            SetExpandedRecursive(false);
        }
        ImGui::EndPopup();
    }

    void FPropertyRow::DrawValueCell(const FPropertyDrawArgs& Args, const FRowDrawState& State)
    {
        const bool bHasExtras = HasExtraControls();
        const bool bHasTrailing = State.bTopLevel && Callbacks.TrailingControlFn;

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2, 0));
        const int32 ColumnCount = 2 + (bHasExtras ? 1 : 0) + (bHasTrailing ? 1 : 0);
        if (!ImGui::BeginTable("EditorRow", ColumnCount, ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::PopStyleVar();
            return;
        }

        ImGui::TableSetupColumn("##Editor", ImGuiTableColumnFlags_WidthStretch);
        if (bHasExtras)
        {
            ImGui::TableSetupColumn("##Extra", ImGuiTableColumnFlags_WidthFixed, GetExtraControlsSectionWidth());
        }
        if (bHasTrailing)
        {
            ImGui::TableSetupColumn("##Trailing", ImGuiTableColumnFlags_WidthFixed, Callbacks.TrailingControlWidth);
        }
        ImGui::TableSetupColumn("##Reset", ImGuiTableColumnFlags_WidthFixed, ResetColumnWidth);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        if (State.bValuesLocked)
        {
            ImGui::TextDisabled("(Multiple Values)");
        }
        else
        {
            const ImVec2 CellMin = ImGui::GetCursorScreenPos();
            const float CellWidth = ImGui::GetContentRegionAvail().x;
            DrawEditor(Args);
            if (State.bMultipleValues)
            {
                // Shaded over rather than drawn with lowered alpha, which a combo's popup would inherit.
                ImGui::GetWindowDrawList()->AddRectFilled(CellMin, ImVec2(CellMin.x + CellWidth, ImGui::GetItemRectMax().y),
                    ImGui::GetColorU32(ImGuiCol_WindowBg, 0.55f));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGuiX::TextTooltip_Internal("The selected objects differ here. Editing sets them all.");
                }
            }
        }

        if (bHasExtras)
        {
            ImGui::TableNextColumn();
            DrawExtraControlsSection();
        }

        if (bHasTrailing)
        {
            ImGui::TableNextColumn();
            Callbacks.TrailingControlFn(PropertyHandle->Property);
        }

        ImGui::TableNextColumn();
        if (State.bDiffers && State.bEditable)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
            if (ImGuiX::FlatButton(LE_ICON_REFRESH "##Reset", ImVec2(ResetColumnWidth - 2, 22), ModifiedMarkerColor()))
            {
                PerformResetToDefault();
            }
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGuiX::TextTooltip_Internal("Reset to default");
            }
        }

        ImGui::EndTable();
        ImGui::PopStyleVar();
    }

    void FPropertyRow::DrawChildren(float ChildOffset, const FPropertyDrawArgs& Args)
    {
        for (const TUniquePtr<FPropertyRow>& Row : Children)
        {
            if (!Row->PassesFilter() || !Row->IsVisible())
            {
                continue;
            }
            Row->DrawRow(ChildOffset, Args);
        }
    }

    FStringView FPropertyRow::GetFilterLabel() const
    {
        // Elements are labeled by index, which nobody searches.
        if (IsArrayElement() || PropertyHandle == nullptr || PropertyHandle->Property == nullptr)
        {
            return FStringView();
        }
        const FCStringView Display = PropertyHandle->Property->GetPropertyDisplayName();
        return FStringView(Display.c_str(), Display.size());
    }

    bool FPropertyRow::ApplyFilter(const ImGuiTextFilter& Filter, bool bFilterActive, bool bJustActivated, bool bJustCleared, bool bAncestorMatched)
    {
        // Without this a search would silently expand the whole tree and leave it that way.
        if (bJustActivated)
        {
            bExpandedSaved = bExpanded;
        }

        const FStringView Label = GetFilterLabel();
        const bool bSelfMatches = bFilterActive && !Label.empty() && ImGuiX::PassSearchFilter(Filter, Label);

        // Not short-circuited, since every descendant needs its own flag updated.
        bool bDescendantMatches = false;
        for (const TUniquePtr<FPropertyRow>& Child : Children)
        {
            bDescendantMatches |= Child->ApplyFilter(Filter, bFilterActive, bJustActivated, bJustCleared, bAncestorMatched || bSelfMatches);
        }

        if (!bFilterActive)
        {
            bPassesFilter = true;
            if (bJustCleared)
            {
                bExpanded = bExpandedSaved;
            }
            return false;
        }

        // A matching row keeps its whole subtree, and only a match further down opens the rows on the way to it.
        bPassesFilter = bAncestorMatched || bSelfMatches || bDescendantMatches;
        if (bDescendantMatches)
        {
            bExpanded = true;
        }
        return bSelfMatches || bDescendantMatches;
    }

    bool FPropertyRow::IsReadOnly() const
    {
        if (PropertyHandle == nullptr)
        {
            return false;
        }
        if (EditCondition.IsBound() && !EditCondition.Evaluate(GetEditConditionContainer()))
        {
            return true;
        }
        return PropertyHandle->Property->IsReadOnly();
    }

    bool FPropertyRow::HasExtraControls() const
    {
        if (!bArrayElement)
        {
            return false;
        }

        // No menu for arrays locked against both resize and reorder.
        const FArrayPropertyRow* ArrayRow = static_cast<const FArrayPropertyRow*>(ParentRow);
        return ArrayRow->AllowResize() || ArrayRow->AllowReorder();
    }

    float FPropertyRow::GetExtraControlsSectionWidth() const
    {
        if (!bArrayElement)
        {
            return 0.0f;
        }

        // Up and down for a reorderable array, delete for a resizable one, then the options menu.
        const FArrayPropertyRow* ArrayRow = static_cast<const FArrayPropertyRow*>(ParentRow);
        const int32 Buttons = 1 + (ArrayRow->AllowReorder() ? 2 : 0) + (ArrayRow->AllowResize() ? 1 : 0);
        return Buttons * ElementButtonWidth + (Buttons - 1) * ImGui::GetStyle().ItemSpacing.x + 4.0f;
    }

    void FPropertyRow::DrawExtraControlsSection()
    {
        FArrayPropertyRow* ArrayRow = static_cast<FArrayPropertyRow*>(ParentRow);
        const int64 Index = PropertyHandle->Index;
        const bool bAllowResize = ArrayRow->AllowResize();
        const bool bAllowReorder = ArrayRow->AllowReorder();
        const bool bCanMoveUp = bAllowReorder && Index > 0;
        const bool bCanMoveDown = bAllowReorder && Index + 1 < ArrayRow->GetElementCount();

        if (bAllowReorder)
        {
            if (ElementButton(LE_ICON_ARROW_UP, bCanMoveUp, "Move element up"))
            {
                ArrayRow->QueueMoveElement(Index, Index - 1);
            }
            ImGui::SameLine();
            if (ElementButton(LE_ICON_ARROW_DOWN, bCanMoveDown, "Move element down"))
            {
                ArrayRow->QueueMoveElement(Index, Index + 1);
            }
            ImGui::SameLine();
        }
        if (bAllowResize)
        {
            if (ElementButton(LE_ICON_TRASH_CAN, true, "Remove element"))
            {
                ArrayRow->QueueRemoveElement(Index);
            }
            ImGui::SameLine();
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
        ImGuiX::FlatButton(LE_ICON_DOTS_HORIZONTAL, ImVec2(ElementButtonWidth, 24), ArrayControlSeed);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Array Element Options");
        }

        if (!ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft))
        {
            return;
        }
        if (bAllowResize && ImGui::MenuItem(LE_ICON_PLUS " Insert New Element"))
        {
            ArrayRow->QueueInsertElement(Index);
        }
        if (bCanMoveUp && ImGui::MenuItem(LE_ICON_ARROW_UP " Move Element Up"))
        {
            ArrayRow->QueueMoveElement(Index, Index - 1);
        }
        if (bCanMoveDown && ImGui::MenuItem(LE_ICON_ARROW_DOWN " Move Element Down"))
        {
            ArrayRow->QueueMoveElement(Index, Index + 1);
        }
        if (bAllowResize && ImGui::MenuItem(LE_ICON_TRASH_CAN " Remove Element"))
        {
            ArrayRow->QueueRemoveElement(Index);
        }
        ImGui::EndPopup();
    }

    FLeafPropertyRow::FLeafPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InPropHandle, InParentRow, InCallbacks)
    {
        Customization = MakeLeafPropertyCustomization(PropertyHandle->Property);
    }

    void FLeafPropertyRow::Update()
    {
        DispatchChange(ChangeOp);
        ChangeOp = EPropertyChangeOp::None;
    }

    void FLeafPropertyRow::DrawHeader(const FPropertyDrawArgs& Args)
    {
        bool bClipped = false;
        if (IsArrayElement())
        {
            ImGui::Text("%lld", static_cast<long long>(PropertyHandle->Index));
        }
        else
        {
            bClipped = DrawHeaderLabel(PropertyHandle->Property->GetPropertyDisplayName().c_str());
        }

        DrawRowTooltip(PropertyHandle.get(), PropertyHandle->Property, bClipped);
    }

    void FLeafPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        ImGui::BeginDisabled(IsReadOnly());

        if (Customization)
        {
            ChangeOp = Customization->UpdateAndDraw(PropertyHandle, Args);
        }
        else
        {
            ImGui::TextUnformatted(LE_ICON_EXCLAMATION " Missing Property Customization");
        }

        ImGui::EndDisabled();
    }

    float FLeafPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        if (IsArrayElement())
        {
            char Buf[32];
            snprintf(Buf, sizeof(Buf), "%lld", static_cast<long long>(PropertyHandle->Index));
            return ImGui::CalcTextSize(Buf).x;
        }
        return ImGui::CalcTextSize(PropertyHandle->Property->GetPropertyDisplayName().c_str()).x;
    }

    FArrayPropertyRow::FArrayPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InPropHandle, InParentRow, InCallbacks)
        , ArrayProperty(static_cast<FArrayProperty*>(InPropHandle->Property))
    {
        RebuildChildren();
    }

    void FArrayPropertyRow::Update()
    {
        ChangeOp = EPropertyChangeOp::None;

        if (!PendingMutations.empty())
        {
            // Started fires before the mutation, so an undo snapshot captures the array as it was.
            DispatchChange(EPropertyChangeOp::Started);
            CaptureElementStates();
            for (const TFunction<void()>& Mutation : PendingMutations)
            {
                Mutation();
            }
            PendingMutations.clear();
            RebuildChildren();
            DispatchChange(EPropertyChangeOp::Finished);
        }

        // Game code can resize the array and a scroll leaves rows far behind, so the window is clamped before this frame draws.
        const int32 Count = GetElementCount();
        const int32 WindowEnd = WindowBegin + (int32)Children.size();
        if (Children.empty() || WindowEnd > Count)
        {
            SetWindow(0, Math::Min(Count, ComplexArrayDisplayLimit));
        }
        else if ((int32)Children.size() > MaxLiveElementRows && IsInnerFixedHeight())
        {
            SetWindow(Math::Max(0, LastVisibleBegin - ElementRowMargin), Math::Min(Count, LastVisibleEnd + ElementRowMargin));
        }
    }

    void FArrayPropertyRow::QueueMutation(TFunction<void()> Mutation)
    {
        PendingMutations.push_back(Move(Mutation));
    }

    void FArrayPropertyRow::CaptureElementStates()
    {
        // Live rows hold the newest state, while elements outside the window keep whatever was saved for them.
        const int32 Needed = Math::Max(GetElementCount(), WindowBegin + (int32)Children.size());
        if ((int32)ElementStates.size() < Needed)
        {
            ElementStates.resize(Needed);
        }
        for (int32 i = 0; i < (int32)Children.size(); ++i)
        {
            FRowExpansionState& State = ElementStates[WindowBegin + i];
            State.clear();
            Children[i]->CaptureExpansion(State);
        }
    }

    void FArrayPropertyRow::SyncChildren()
    {
        if (GetPropertyHandle()->GetValuePtr() != BuiltArray)
        {
            CaptureElementStates();
            RebuildChildren();
        }
    }

    void FArrayPropertyRow::DrawHeader(const FPropertyDrawArgs& Args)
    {
        bool bClipped = false;
        const ImGuiTreeNodeFlags Flags = GetElementCount() > 0 ? 0 : ImGuiTreeNodeFlags_Leaf;
        bExpanded = DrawRowFoldout(ArrayProperty->GetPropertyDisplayName().c_str(), bExpanded, Flags, bClipped);
        DrawPropertyTooltip(ArrayProperty, bClipped);
    }

    void FArrayPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        ImGui::TextColored(EditorColors::TextMuted(), "%d Elements", GetElementCount());
    }

    float FArrayPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        return ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(ArrayProperty->GetPropertyDisplayName().c_str()).x;
    }

    bool FArrayPropertyRow::IsInnerFixedHeight() const
    {
        if (ArrayProperty == nullptr)
        {
            return false;
        }

        FProperty* Inner = ArrayProperty->GetInternalProperty();
        if (Inner == nullptr)
        {
            return false;
        }

        return IsFixedHeightPropertyType(Inner->GetType());
    }

    void FArrayPropertyRow::DrawChildren(float ChildOffset, const FPropertyDrawArgs& Args)
    {
        const int32 Count = GetElementCount();
        if (Count == 0)
        {
            return;
        }

        if (IsInnerFixedHeight())
        {
            // A known row height skips the clipper's measuring pass, which would otherwise pull element 0 into a scrolled window.
            ImGuiListClipper Clipper;
            Clipper.Begin(Count, ElementRowHeight);
            int32 Widest = 0;
            while (Clipper.Step())
            {
                CoverElements(Clipper.DisplayStart, Clipper.DisplayEnd);
                for (int32 i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                {
                    Children[i - WindowBegin]->DrawRow(ChildOffset, Args);
                }
                if (Clipper.DisplayEnd - Clipper.DisplayStart > Widest)
                {
                    Widest = Clipper.DisplayEnd - Clipper.DisplayStart;
                    LastVisibleBegin = Clipper.DisplayStart;
                    LastVisibleEnd = Clipper.DisplayEnd;
                }
            }
            if (Clipper.ItemsHeight > 0.0f)
            {
                ElementRowHeight = Clipper.ItemsHeight;
            }
            return;
        }

        const int32 DisplayCount = bShowAllElements ? Count : Math::Min(Count, ComplexArrayDisplayLimit);
        CoverElements(0, DisplayCount);
        for (int32 i = 0; i < DisplayCount; ++i)
        {
            Children[i - WindowBegin]->DrawRow(ChildOffset, Args);
        }

        if (DisplayCount < Count)
        {
            DrawTruncationRow(ChildOffset, Count - DisplayCount);
        }
    }

    void FArrayPropertyRow::DrawTruncationRow(float Offset, int32 HiddenCount)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();
        ImGui::TextDisabled("%d more element%s hidden", HiddenCount, HiddenCount == 1 ? "" : "s");

        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Show All"))
        {
            bShowAllElements = true;
        }
    }

    void FArrayPropertyRow::RebuildChildren()
    {
        BuiltArray = GetPropertyHandle()->GetValuePtr();
        DestroyChildren();
        WindowBegin = 0;
        bShowAllElements = false;
        SetWindow(0, Math::Min(GetElementCount(), ComplexArrayDisplayLimit));
    }

    int32 FArrayPropertyRow::GetElementCount() const
    {
        const void* Array = PropertyHandle->GetValuePtr();
        return Array != nullptr ? (int32)ArrayProperty->GetNum(Array) : 0;
    }

    void FArrayPropertyRow::SetExpandedRecursive(bool bExpand)
    {
        // Saved states would override the choice on every element created afterwards.
        ElementStates.clear();
        ElementExpansion = bExpand;
        FPropertyRow::SetExpandedRecursive(bExpand);
    }

    TUniquePtr<FPropertyRow> FArrayPropertyRow::CreateElementRow(int32 Index)
    {
        TSharedPtr<FPropertyHandle> ElementPropHandle = MakeShared<FPropertyHandle>(
            ArrayProperty,
            GetPropertyHandle()->GetValuePtr(),
            GetPropertyHandle()->GetDefaultValuePtr(),
            ArrayProperty->GetInternalProperty(),
            static_cast<int64>(Index));
        TUniquePtr<FPropertyRow> NewRow = CreatePropertyRow(ElementPropHandle, this, Callbacks);
        NewRow->SetIsArrayElement(true);
        if (ElementExpansion.has_value())
        {
            NewRow->SetExpandedRecursive(*ElementExpansion);
        }
        if (Index < (int32)ElementStates.size())
        {
            NewRow->RestoreExpansion(ElementStates[Index]);
        }
        return NewRow;
    }

    void FArrayPropertyRow::QueueMoveElement(int64 Index, int64 Target)
    {
        QueueMutation([this, Index, Target]
        {
            void* Array = PropertyHandle->GetValuePtr();
            const int64 Num = GetElementCount();
            if (Array == nullptr || Index < 0 || Index >= Num || Target < 0 || Target >= Num)
            {
                return;
            }
            ArrayProperty->Swap(Array, Index, Target);
            if (Index < (int64)ElementStates.size() && Target < (int64)ElementStates.size())
            {
                std::swap(ElementStates[Index], ElementStates[Target]);
            }
        });
    }

    void FArrayPropertyRow::QueueInsertElement(int64 Index)
    {
        QueueMutation([this, Index]
        {
            void* Array = PropertyHandle->GetValuePtr();
            const int64 Num = GetElementCount();
            if (Array == nullptr || Index < 0 || Index > Num)
            {
                return;
            }
            ArrayProperty->PushBack(Array, nullptr);
            for (int64 i = Num; i > Index; --i)
            {
                ArrayProperty->Swap(Array, i, i - 1);
            }
            if (Index <= (int64)ElementStates.size())
            {
                ElementStates.insert(ElementStates.begin() + Index, FRowExpansionState());
            }
        });
    }

    void FArrayPropertyRow::QueueRemoveElement(int64 Index)
    {
        QueueMutation([this, Index]
        {
            void* Array = PropertyHandle->GetValuePtr();
            if (Array == nullptr || Index < 0 || Index >= GetElementCount())
            {
                return;
            }
            ArrayProperty->RemoveAt(Array, Index);
            if (Index < (int64)ElementStates.size())
            {
                ElementStates.erase(ElementStates.begin() + Index);
            }
        });
    }

    void FArrayPropertyRow::SetWindow(int32 Begin, int32 End)
    {
        const int32 OldBegin = WindowBegin;
        const int32 OldEnd = WindowBegin + (int32)Children.size();

        // Rows inside both windows move across, so one drawn earlier this frame stays alive.
        TVector<TUniquePtr<FPropertyRow>> Rows;
        Rows.reserve(Math::Max(0, End - Begin));
        for (int32 i = Begin; i < End; ++i)
        {
            Rows.push_back(i >= OldBegin && i < OldEnd ? Move(Children[i - OldBegin]) : CreateElementRow(i));
        }
        Children = Move(Rows);
        WindowBegin = Begin;
    }

    void FArrayPropertyRow::CoverElements(int32 Begin, int32 End)
    {
        const int32 WindowEnd = WindowBegin + (int32)Children.size();
        if (Begin >= WindowBegin && End <= WindowEnd)
        {
            return;
        }

        int32 NewBegin = Math::Min(Begin, WindowBegin);
        int32 NewEnd = Math::Max(End, WindowEnd);
        if (Children.empty() || NewEnd - NewBegin > MaxLiveElementRows)
        {
            NewBegin = Begin;
            NewEnd = End;
        }
        SetWindow(NewBegin, NewEnd);
    }

    bool FArrayPropertyRow::AllowResize() const
    {
        // Disabling only the value widgets left the add and trash buttons live, a bigger edit than refused.
        return !bDrawReadOnly && (ArrayProperty == nullptr || !ArrayProperty->HasEditorFlag(EPropertyEditorFlags::NoResize));
    }

    bool FArrayPropertyRow::AllowReorder() const
    {
        return !bDrawReadOnly && (ArrayProperty == nullptr || !ArrayProperty->HasEditorFlag(EPropertyEditorFlags::NoReorder));
    }

    bool FArrayPropertyRow::HasExtraControls() const
    {
        // Both extra controls are resize operations, so a NoResize array hides them entirely.
        return AllowResize();
    }

    float FArrayPropertyRow::GetExtraControlsSectionWidth() const
    {
        return ContainerControlsWidth();
    }

    void FArrayPropertyRow::DrawExtraControlsSection()
    {
        if (ElementButton(LE_ICON_PLUS, true, "Add array element"))
        {
            QueueMutation([this]
            {
                if (void* Array = PropertyHandle->GetValuePtr())
                {
                    ArrayProperty->PushBack(Array, nullptr);
                }
            });
        }
        ImGui::SameLine();
        if (ElementButton(LE_ICON_TRASH_CAN, true, "Remove all array elements"))
        {
            QueueMutation([this]
            {
                if (void* Array = PropertyHandle->GetValuePtr())
                {
                    ArrayProperty->Clear(Array);
                    ElementStates.clear();
                }
            });
        }
    }

    FMapPropertyRow::FMapPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InPropHandle, InParentRow, InCallbacks)
        , MapProperty(static_cast<FMapProperty*>(InPropHandle->Property))
    {
        RebuildChildren();
    }

    void FMapPropertyRow::Update()
    {
        ChangeOp = EPropertyChangeOp::None;

        if (PendingMutations.empty())
        {
            return;
        }

        DispatchChange(EPropertyChangeOp::Started);
        for (const TFunction<void()>& Mutation : PendingMutations)
        {
            Mutation();
        }
        PendingMutations.clear();
        RebuildChildren();
        DispatchChange(EPropertyChangeOp::Finished);
    }

    void FMapPropertyRow::QueueMutation(TFunction<void()> Mutation)
    {
        PendingMutations.push_back(Move(Mutation));
    }

    void FMapPropertyRow::QueueRemoveEntry(int64 Index)
    {
        QueueMutation([this, Index]
        {
            void* Container = PropertyHandle->GetValuePtr();
            if (Container == nullptr || Index < 0 || static_cast<size_t>(Index) >= MapProperty->GetNum(Container))
            {
                return;
            }

            // Copied out first, since the key it names lives in the slot the removal destroys.
            FMapKeyScratch Key(MapProperty);
            MapProperty->GetKeyProperty()->CopyCompleteValue(Key.Get(), MapProperty->GetKeyAt(Container, static_cast<size_t>(Index)));
            MapProperty->RemoveByKey(Container, Key.Get());
        });
    }

    void FMapPropertyRow::DrawHeader(const FPropertyDrawArgs& Args)
    {
        bool bClipped = false;
        const ImGuiTreeNodeFlags Flags = Children.empty() ? ImGuiTreeNodeFlags_Leaf : 0;
        bExpanded = DrawRowFoldout(MapProperty->GetPropertyDisplayName().c_str(), bExpanded, Flags, bClipped);
        DrawPropertyTooltip(MapProperty, bClipped);
    }

    void FMapPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        ImGui::TextColored(EditorColors::TextMuted(), "%llu Entries", static_cast<unsigned long long>(Children.size()));
    }

    float FMapPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        return ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(MapProperty->GetPropertyDisplayName().c_str()).x;
    }

    void FMapPropertyRow::SyncChildren()
    {
        void* Container = PropertyHandle->GetValuePtr();
        const size_t Count = Container != nullptr ? MapProperty->GetNum(Container) : 0;
        if (Container != BuiltMap || Count != Children.size())
        {
            RebuildChildren();
        }
    }

    void FMapPropertyRow::RebuildChildren()
    {
        DestroyChildren();

        void* Container = GetPropertyHandle()->GetValuePtr();
        BuiltMap = Container;
        if (Container == nullptr || MapProperty == nullptr)
        {
            return;
        }

        const size_t Count = MapProperty->GetNum(Container);
        Children.reserve(Count);
        for (size_t i = 0; i < Count; ++i)
        {
            Children.push_back(MakeUnique<FMapEntryRow>(this, static_cast<int64>(i), Callbacks));
        }
    }

    bool FMapPropertyRow::AllowResize() const
    {
        return !bDrawReadOnly && (MapProperty == nullptr || !MapProperty->HasEditorFlag(EPropertyEditorFlags::NoResize));
    }

    bool FMapPropertyRow::HasExtraControls() const
    {
        return AllowResize();
    }

    float FMapPropertyRow::GetExtraControlsSectionWidth() const
    {
        return ContainerControlsWidth();
    }

    void FMapPropertyRow::DrawExtraControlsSection()
    {
        if (ElementButton(LE_ICON_PLUS, true, "Add map entry"))
        {
            QueueMutation([this]
            {
                if (void* Container = PropertyHandle->GetValuePtr())
                {
                    // A colliding default key is a no-op, since Insert is insert-or-assign.
                    FMapKeyScratch Key(MapProperty);
                    MapProperty->Insert(Container, Key.Get(), nullptr);
                }
            });
        }
        ImGui::SameLine();
        if (ElementButton(LE_ICON_TRASH_CAN, true, "Remove all map entries"))
        {
            QueueMutation([this]
            {
                if (void* Container = PropertyHandle->GetValuePtr())
                {
                    MapProperty->Clear(Container);
                }
            });
        }
    }

    FMapKeyScratch::FMapKeyScratch(FMapProperty* Map)
        : KeyProperty(Map->GetKeyProperty())
    {
        const uint32 Size = Math::Max(Map->GetKeySize(), 1u);
        Memory = Memory::Malloc(Size, 16);
        Memory::Memzero(Memory, Size);
        KeyProperty->ConstructValue(Memory);
    }

    FMapKeyScratch::~FMapKeyScratch()
    {
        KeyProperty->DestructValue(Memory);
        Memory::Free(Memory);
    }

    FMapEntryRow::FMapEntryRow(FMapPropertyRow* InMapRow, int64 InIndex, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(MakeMapValueHandle(InMapRow, InIndex), InMapRow, InCallbacks)
        , MapRow(InMapRow)
        , Map(InMapRow->GetMapProperty())
        , KeyProperty(Map->GetKeyProperty())
        , MapContainer(InMapRow->GetPropertyHandle()->GetValuePtr())
        , PreviousKey(Map)
        , EntryIndex(InIndex)
    {
        KeyHandle = MakeShared<FPropertyHandle>(Map, MapContainer, MapRow->GetPropertyHandle()->GetDefaultValuePtr(), KeyProperty, InIndex, true);
        KeyCustomization = MakeLeafPropertyCustomization(KeyProperty);

        // A value with member rows (a struct, array or map) has nothing to draw inline, so it nests below the key.
        TUniquePtr<FPropertyRow> Row = CreatePropertyRow(PropertyHandle, this, Callbacks);
        if (Row->HasCustomization())
        {
            ValueRow = Move(Row);
        }
        else
        {
            Children.push_back(Move(Row));
        }
    }

    void FMapEntryRow::CaptureKeySnapshot()
    {
        if (const void* Slot = KeyHandle->GetValuePtr())
        {
            KeyProperty->CopyCompleteValue(PreviousKey.Get(), Slot);
        }
    }

    bool FMapEntryRow::KeyDuplicatesAnotherEntry() const
    {
        const void* MyKey = KeyHandle->GetValuePtr();
        if (MyKey == nullptr)
        {
            return false;
        }
        const size_t Count = Map->GetNum(MapContainer);
        for (size_t j = 0; j < Count; ++j)
        {
            if (static_cast<int64>(j) == EntryIndex)
            {
                continue;
            }
            const void* OtherKey = Map->GetKeyAt(MapContainer, j);
            if (OtherKey != nullptr && KeyProperty->Identical(MyKey, OtherKey))
            {
                return true;
            }
        }
        return false;
    }

    void FMapEntryRow::Update()
    {
        if (ValueRow)
        {
            ValueRow->UpdateRow();
        }

        const EPropertyChangeOp Op = KeyChangeOp;
        KeyChangeOp = EPropertyChangeOp::None;
        if (Op == EPropertyChangeOp::None || KeyCustomization == nullptr)
        {
            return;
        }

        // Taken before the edit's first write, so a duplicate has the committed key to revert to.
        if (!bKeyEditSessionActive)
        {
            CaptureKeySnapshot();
        }

        const bool bCommit = Op == EPropertyChangeOp::Finished || (Op == EPropertyChangeOp::Updated && !bKeyEditSessionActive);
        DispatchPropertyChange(Op, bKeyEditSessionActive, Callbacks, Map, MapContainer, [this, bCommit]
        {
            KeyCustomization->UpdatePropertyValue(KeyHandle);

            // A key duplicating another entry would silently collapse on reload, so the commit reverts it.
            if (bCommit && KeyDuplicatesAnotherEntry())
            {
                if (void* Slot = KeyHandle->GetValuePtr())
                {
                    KeyProperty->CopyCompleteValue(Slot, PreviousKey.Get());
                }
                KeyCustomization->HandleExternalUpdate(KeyHandle);
                ImGuiX::Notifications::NotifyWarning("Duplicate map key ignored; keys must be unique.");
            }
        });
    }

    void FMapEntryRow::DrawHeader(const FPropertyDrawArgs& Args)
    {
        if (KeyCustomization)
        {
            KeyChangeOp = KeyCustomization->UpdateAndDraw(KeyHandle, FPropertyDrawArgs{ Args.bReadOnly || IsReadOnly(), Args.Context });
        }
        else
        {
            // A non-leaf key gets a read-only placeholder and is edited through add and remove.
            ImGui::TextDisabled("<key %lld>", static_cast<long long>(EntryIndex));
        }
    }

    void FMapEntryRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        if (ValueRow)
        {
            ValueRow->DrawEditor(Args);
        }
    }

    float FMapEntryRow::GetMeasuredHeaderTextWidth() const
    {
        return ImGui::GetFontSize() * 8.0f;
    }

    bool FMapEntryRow::HasExtraControls() const
    {
        return MapRow->AllowResize();
    }

    float FMapEntryRow::GetExtraControlsSectionWidth() const
    {
        return ElementButtonWidth + 4.0f;
    }

    void FMapEntryRow::DrawExtraControlsSection()
    {
        if (ElementButton(LE_ICON_TRASH_CAN, true, "Remove entry"))
        {
            MapRow->QueueRemoveEntry(EntryIndex);
        }
    }

    FStructPropertyRow::FStructPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InPropHandle, InParentRow, InCallbacks)
        , StructProperty(static_cast<FStructProperty*>(InPropHandle->Property))
    {
        FPropertyCustomizationRegistry* Registry = GEngine->GetDevelopmentToolsUI()->GetPropertyCustomizationRegistry();
        Customization = Registry->GetPropertyCustomizationForType(StructProperty->GetStruct()->GetName());
        
        if (StructProperty->HasEditorFlag(EPropertyEditorFlags::DefaultCollapsed))
        {
            bExpanded = false;
        }

        RebuildChildren();
    }

    void FStructPropertyRow::Update()
    {
        DispatchChange(ChangeOp);
        ChangeOp = EPropertyChangeOp::None;
    }

    void FStructPropertyRow::SyncChildren()
    {
        if (Customization == nullptr && PropertyHandle->GetValuePtr() != BuiltValue)
        {
            RebuildKeepingExpansion([this] { RebuildChildren(); });
        }
    }

    FFixedString FStructPropertyRow::GetHeaderLabel() const
    {
        if (!IsArrayElement())
        {
            return FFixedString(StructProperty->GetPropertyDisplayName().c_str());
        }

        char Index[32];
        snprintf(Index, sizeof(Index), "[%lld]", static_cast<long long>(PropertyHandle->Index));
        FFixedString Label(Index);
        const FArrayPropertyRow* ArrayRow = static_cast<const FArrayPropertyRow*>(ParentRow);
        FProperty* ArrayProperty = ArrayRow->GetPropertyHandle()->Property;
        FCStringView TitleName = ArrayProperty->GetMetadata("TitleProperty");
        if (TitleName.empty())
        {
            TitleName = "Name";
        }

        FProperty* TitleProperty = StructProperty->GetStruct()->GetProperty(FName(FStringView(TitleName.c_str(), TitleName.size())));
        void* StructValue = PropertyHandle->GetValuePtr();
        if (TitleProperty == nullptr || StructValue == nullptr)
        {
            return Label;
        }

        const char* Title = nullptr;
        if (TitleProperty->GetType() == EPropertyTypeFlags::Name)
        {
            const FName* Value = TitleProperty->GetValuePtr<FName>(StructValue);
            Title = Value->IsNone() ? nullptr : Value->c_str();
        }
        else if (TitleProperty->GetType() == EPropertyTypeFlags::String)
        {
            Title = TitleProperty->GetValuePtr<FString>(StructValue)->c_str();
        }

        if (Title != nullptr && Title[0] != 0)
        {
            Label += "  ";
            Label += Title;
        }
        return Label;
    }

    void FStructPropertyRow::DrawHeader(const FPropertyDrawArgs& Args)
    {
        const FFixedString Label = GetHeaderLabel();

        // A customization draws the whole value in one row, so there is nothing for an arrow to fold.
        bool bClipped = false;
        if (Customization)
        {
            bClipped = DrawHeaderLabel(Label.c_str());
        }
        else
        {
            bExpanded = DrawRowFoldout(Label.c_str(), bExpanded, 0, bClipped, this);
        }
        DrawRowTooltip(PropertyHandle.get(), StructProperty, bClipped, FStringView(Label.c_str(), Label.size()));
    }

    void FStructPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        if (Customization)
        {
            ImGui::BeginDisabled(IsReadOnly());
            ChangeOp = Customization->UpdateAndDraw(PropertyHandle, Args);
            ImGui::EndDisabled();
        }
    }

    float FStructPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        const FFixedString Label = GetHeaderLabel();
        const float LabelWidth = ImGui::CalcTextSize(Label.c_str()).x;
        return Customization ? LabelWidth : ImGui::GetTreeNodeToLabelSpacing() + LabelWidth;
    }

    void FStructPropertyRow::RebuildChildren()
    {
        BuiltValue = PropertyHandle->GetValuePtr();

        // A customization draws the whole value itself, so member rows would show it twice.
        if (Customization)
        {
            return;
        }
        BuildMemberRows(StructProperty->GetStruct(), BuiltValue, PropertyHandle->GetDefaultValuePtr());
    }

    FInstancedStructPropertyRow::FInstancedStructPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InPropHandle, InParentRow, InCallbacks)
        , InstancedStructProperty(static_cast<FInstancedStructProperty*>(InPropHandle->Property))
    {
        if (InstancedStructProperty->HasEditorFlag(EPropertyEditorFlags::DefaultCollapsed))
        {
            bExpanded = false;
        }

        RebuildChildren();
    }

    void FInstancedStructPropertyRow::Update()
    {
        DispatchChange(ChangeOp);
        ChangeOp = EPropertyChangeOp::None;
    }

    void FInstancedStructPropertyRow::SyncChildren()
    {
        // Loading or assigning the value reallocates its memory even when the type stays, as every undo does.
        auto* Instance = static_cast<FInstancedStruct*>(PropertyHandle->GetValuePtr());
        void* Memory = Instance ? Instance->GetMutableMemory() : nullptr;
        CStruct* Type = Instance ? Instance->GetScriptStruct() : nullptr;
        if (Type == CachedType && Memory == BuiltMemory)
        {
            return;
        }

        if (Type != CachedType)
        {
            RebuildChildren();
        }
        else
        {
            RebuildKeepingExpansion([this] { RebuildChildren(); });
        }
    }

    void FInstancedStructPropertyRow::DrawHeader(const FPropertyDrawArgs& Args)
    {
        bool bClipped = false;
        bExpanded = DrawRowFoldout(InstancedStructProperty->GetPropertyDisplayName().c_str(), bExpanded, 0, bClipped);
        DrawRowTooltip(PropertyHandle.get(), InstancedStructProperty, bClipped);
    }

    void FInstancedStructPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        auto* Instance = static_cast<FInstancedStruct*>(PropertyHandle->GetValuePtr());
        if (Instance == nullptr)
        {
            return;
        }

        // Type picker on the header line; visible whether expanded or not so the type is always changeable.
        ImGui::BeginDisabled(Args.bReadOnly || IsReadOnly());

        bool bChanged = false;
        CStruct* Current = Instance->GetScriptStruct();
        const FCStringView Implements = InstancedStructProperty->GetMetadata("Implements");
        CStruct* Picked = DrawInstancedStructTypePicker("##instancedstructpicker", InstancedStructProperty->GetMetaStruct(),
            FStringView(Implements.data(), Implements.size()), Current, bChanged);

        if (bChanged && Picked != Current)
        {
            // A type swap reallocates the instance, so it runs as one transaction and rebuilds the member rows.
            DispatchChange(EPropertyChangeOp::Started);
            Instance->InitializeAs(Picked);
            RebuildChildren();
            DispatchChange(EPropertyChangeOp::Finished);
        }

        ImGui::EndDisabled();
    }

    float FInstancedStructPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        return ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(InstancedStructProperty->GetPropertyDisplayName().c_str()).x;
    }

    void FInstancedStructPropertyRow::RebuildChildren()
    {
        auto* Instance = static_cast<FInstancedStruct*>(PropertyHandle->GetValuePtr());
        BuiltMemory = Instance ? Instance->GetMutableMemory() : nullptr;
        CStruct* Type = Instance ? Instance->GetScriptStruct() : nullptr;
        CachedType = Type;

        if (Instance == nullptr || Type == nullptr)
        {
            DestroyChildren();
            return;
        }

        // Diff/reset of the instance's own fields compares against that struct's default instance.
        BuildMemberRows(Type, BuiltMemory, Type->GetDefaultInstance());
    }

    FOptionalPropertyRow::FOptionalPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InPropHandle, InParentRow, InCallbacks)
        , OptionalProperty(static_cast<FOptionalProperty*>(InPropHandle->Property))
    {
        RebuildChildren();
    }

    void FOptionalPropertyRow::Update()
    {
        DispatchChange(ChangeOp);
        ChangeOp = EPropertyChangeOp::None;
    }

    void FOptionalPropertyRow::SyncChildren()
    {
        void* ContainerPtr = PropertyHandle->GetValuePtr();
        void* Payload = ContainerPtr != nullptr && OptionalProperty->HasValue(ContainerPtr) ? OptionalProperty->GetValue(ContainerPtr) : nullptr;
        if (Payload != BuiltPayload)
        {
            RebuildChildren();
        }
    }

    void FOptionalPropertyRow::DrawHeader(const FPropertyDrawArgs& Args)
    {
        const bool bClipped = DrawHeaderLabel(OptionalProperty->GetPropertyDisplayName().c_str());
        DrawRowTooltip(PropertyHandle.get(), OptionalProperty, bClipped);
    }

    void FOptionalPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        void* ContainerPtr = GetPropertyHandle()->GetValuePtr();
        if (ContainerPtr == nullptr)
        {
            return;
        }
        bool bEngaged = OptionalProperty->HasValue(ContainerPtr);

        ImGui::BeginDisabled(Args.bReadOnly || IsReadOnly());
        if (ImGui::Checkbox("##OptionalSet", &bEngaged))
        {
            // Opened before the write, so an undo snapshot captures the optional as it was.
            DispatchChange(EPropertyChangeOp::Started);
            if (bEngaged)
            {
                OptionalProperty->SetValue(ContainerPtr, nullptr);
            }
            else
            {
                OptionalProperty->Reset(ContainerPtr);
            }
            RebuildChildren();
            DispatchChange(EPropertyChangeOp::Finished);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled(bEngaged ? "set" : "(none)");
    }

    float FOptionalPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        return ImGui::CalcTextSize(OptionalProperty->GetPropertyDisplayName().c_str()).x;
    }

    void FOptionalPropertyRow::RebuildChildren()
    {
        DestroyChildren();
        BuiltPayload = nullptr;

        void* ContainerPtr = GetPropertyHandle()->GetValuePtr();
        void* DefaultContainerPtr = GetPropertyHandle()->GetDefaultValuePtr();
        if (ContainerPtr == nullptr || !OptionalProperty->HasValue(ContainerPtr))
        {
            return;
        }

        FProperty* Inner = OptionalProperty->GetInternalProperty();
        if (Inner == nullptr)
        {
            return;
        }

        // The payload is the child's container, and the default only counts when the default is engaged too.
        void* DefaultPayload = nullptr;
        if (DefaultContainerPtr != nullptr && OptionalProperty->HasValue(DefaultContainerPtr))
        {
            DefaultPayload = OptionalProperty->GetValue(DefaultContainerPtr);
        }
        BuiltPayload = OptionalProperty->GetValue(ContainerPtr);
        AddPropertyRow(MakeShared<FPropertyHandle>(BuiltPayload, DefaultPayload, Inner));
    }

    FCategoryPropertyRow::FCategoryPropertyRow(const FName& InCategory, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InCallbacks)
        , Category(InCategory)
    {
    }

    FStringView FCategoryPropertyRow::GetFilterLabel() const
    {
        return FStringView(Category.c_str());
    }

    void FCategoryPropertyRow::DrawHeader(const FPropertyDrawArgs& Args)
    {
        // A darker background reads as a section break independent of the row-bg alternation.
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, CategoryBgColor());
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, CategoryBgColor());

        ImGui::SetNextItemOpen(bExpanded);
        ImGui::PushStyleColor(ImGuiCol_Header, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorColors::U32(EditorColors::RowBgActive()));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorColors::U32(EditorColors::RowBgHovered()));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextPrimary());
        bool bClipped = false;
        bExpanded = CollapsingHeaderEllipsis(Category.c_str(), ImGuiTreeNodeFlags_DefaultOpen, bClipped);
        ImGui::PopStyleColor(4);

        if (bClipped && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::WrappedTooltip_Internal(FStringView(Category.c_str()));
        }
    }

    float FCategoryPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        return ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(Category.c_str()).x;
    }

}

namespace Lumina
{
    FPropertyTable::FPropertyTable()
        : FPropertyTable(nullptr, nullptr, nullptr)
    {
    }

    FPropertyTable::FPropertyTable(void* InObject, CStruct* InType)
        : FPropertyTable(InObject, InType, PropertyTableInternal::DefaultInstanceFor(InObject, InType))
    {
    }

    FPropertyTable::FPropertyTable(void* InObject, CStruct* InType, void* InDefaultObject)
    {
        Bind(InObject, InType, InDefaultObject);

        // The rows reach into script types, so they go while those types are still alive.
        ScriptsWillReloadHandle = FCoreDelegates::OnScriptsWillReload.AddLambda([this] { DropRows(); });
    }

    FPropertyTable::FPropertyTable(CObject* InObject)
        : FPropertyTable(InObject, InObject != nullptr ? InObject->GetClass() : nullptr)
    {
    }

    FPropertyTable::~FPropertyTable()
    {
        FCoreDelegates::OnScriptsWillReload.Remove(ScriptsWillReloadHandle);
    }

    void FPropertyTable::DrawTree(bool bReadOnly)
    {
        DrawTree(FPropertyDrawArgs{ bReadOnly });
    }

    void FPropertyTable::DrawTree(const FPropertyDrawArgs& InArgs)
    {
        // A table with no context of its own inherits the caller's, which is what nests a struct row.
        const FPropertyDrawArgs Args{ InArgs.bReadOnly, Context != nullptr ? *Context : InArgs.Context };

        EnsureTreeBuilt();
        if (Customization)
        {
            DrawCustomization(Args);
            return;
        }

        ImGui::PushID(this);

        // Search runs first, so a filtered-out row cannot size the header column.
        if (bShowSearchBar)
        {
            ImGuiX::SearchBar("##PropertySearch", PropertyFilter, LE_ICON_MAGNIFY " Search properties...");
        }
        ApplyFilterState();

        float HeaderColumnWidth = 0.0f;
        for (const TUniquePtr<PropertyTableInternal::FCategoryPropertyRow>& Row : Categories)
        {
            if (Row->PassesFilter())
            {
                HeaderColumnWidth = Math::Max(HeaderColumnWidth, Row->ComputeRequiredHeaderWidth(0.0f));
            }
        }

        // Long or deeply nested labels clip and keep their hover tooltip, rather than squeezing the values out.
        const float MaxHeaderWidth = Math::Max(ImGui::GetContentRegionAvail().x * PropertyTableInternal::MaxHeaderColumnShare, 80.0f);
        HeaderColumnWidth = Math::Min(HeaderColumnWidth + 18.0f, MaxHeaderWidth);

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 3));
        ImGui::PushStyleColor(ImGuiCol_TableRowBg, EditorColors::U32(EditorColors::PanelBg()));
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, EditorColors::U32(EditorColors::RowBg()));

        constexpr ImGuiTableFlags Flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_NoBordersInBodyUntilResize
            | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg;
        if (ImGui::BeginTable("GridTable", 2, Flags))
        {
            ImGui::TableSetupColumn("##Header", ImGuiTableColumnFlags_WidthFixed, HeaderColumnWidth);
            ImGui::TableSetupColumn("##Editor", ImGuiTableColumnFlags_WidthStretch);

            const bool bFoldSoleCategory = Categories.size() == 1 && !OwnerHeading.IsNone() && Categories.front()->GetCategoryName() == OwnerHeading;
            for (const TUniquePtr<PropertyTableInternal::FCategoryPropertyRow>& Row : Categories)
            {
                // UpdateRow drains queued array mutations, so skipping it strands an edit made before the search.
                Row->UpdateRow();
                if (!Row->PassesFilter())
                {
                    continue;
                }

                if (bFoldSoleCategory)
                {
                    Row->DrawChildren(0.0f, Args);
                }
                else
                {
                    Row->DrawRow(0.0f, Args);
                }
            }

            ImGui::EndTable();
        }

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        ImGui::PopID();
    }

    void FPropertyTable::UpdateRows()
    {
        if (bDirty || Customization)
        {
            return;
        }
        for (const TUniquePtr<PropertyTableInternal::FCategoryPropertyRow>& Row : Categories)
        {
            Row->UpdateRow();
        }
    }

    void FPropertyTable::MarkDirty()
    {
        bDirty = true;
    }

    void FPropertyTable::SetObject(void* InObject, CStruct* StructType)
    {
        if (Object != InObject || Struct != StructType)
        {
            SetObject(InObject, StructType, PropertyTableInternal::DefaultInstanceFor(InObject, StructType));
        }
    }

    void FPropertyTable::SetObject(void* InObject, CStruct* StructType, void* InDefaultObject)
    {
        // Rebinding drops every row and its expansion state, so pointing at what the table already holds is skipped.
        if (Object == InObject && Struct == StructType && DefaultObject == InDefaultObject)
        {
            return;
        }
        Bind(InObject, StructType, InDefaultObject);
        DropRows();
    }

    void FPropertyTable::SetPreEditCallback(FPropertyChangedEventFn Callback)
    {
        Callbacks.PreChangeCallback = Move(Callback);
    }

    void FPropertyTable::SetPostEditCallback(FPropertyChangedEventFn Callback)
    {
        Callbacks.PostChangeCallback = Move(Callback);
    }

    void FPropertyTable::SetStartEditCallback(FPropertyChangedEventFn Callback)
    {
        Callbacks.StartChangeCallback = Move(Callback);
    }

    void FPropertyTable::SetFinishEditCallback(FPropertyChangedEventFn Callback)
    {
        Callbacks.FinishChangeCallback = Move(Callback);
    }

    void FPropertyTable::SetMultiValueFn(FPropertyMultiValueFn Fn)
    {
        Callbacks.IsMultiValueFn = Move(Fn);
    }

    void FPropertyTable::SetEditMultipleValues(bool bEdit)
    {
        Callbacks.bEditMultipleValues = bEdit;
    }

    void FPropertyTable::SetTrailingControl(FPropertyTrailingControlFn Fn, float Width)
    {
        Callbacks.TrailingControlFn = Move(Fn);
        Callbacks.TrailingControlWidth = Width;
    }

    void FPropertyTable::SetSearchText(FStringView Text)
    {
        const size_t Count = Math::Min(Text.size(), sizeof(PropertyFilter.InputBuf) - 1);
        if (Count > 0)
        {
            Memory::Memcpy(PropertyFilter.InputBuf, Text.data(), Count);
        }
        PropertyFilter.InputBuf[Count] = 0;
        PropertyFilter.Build();
    }

    bool FPropertyTable::PrepareAndTestFilter()
    {
        EnsureTreeBuilt();

        // A customization draws its own table, so it is always shown rather than vanishing under a search.
        if (Customization != nullptr)
        {
            return true;
        }

        ApplyFilterState();
        if (!PropertyFilter.IsActive())
        {
            return true;
        }

        for (const TUniquePtr<PropertyTableInternal::FCategoryPropertyRow>& Row : Categories)
        {
            if (Row->PassesFilter())
            {
                return true;
            }
        }
        return false;
    }

    void FPropertyTable::OnScriptsReloaded(const Scripting::FScriptReload& Reload)
    {
        // A replaced type may already be freed, so it is only compared and its replacement is what gets read.
        CStruct* ReplacedType = static_cast<CStruct*>(Reload.FindReplacement(Struct));
        CStruct* LiveType = ReplacedType != nullptr ? ReplacedType : Struct;

        // Only an object table can be reinstanced, so a struct table's value pointer is never looked up as one.
        const bool bObjectTable = LiveType != nullptr && LiveType->IsA<CClass>();
        if (CObject* Replacement = bObjectTable ? Reload.FindReplacement(Object) : nullptr)
        {
            Bind(Replacement, Replacement->GetClass(), PropertyTableInternal::DefaultInstanceFor(Replacement, Replacement->GetClass()));
        }
        else if (ReplacedType != nullptr)
        {
            Bind(Object, ReplacedType, DefaultObject);
        }

        DropRows();
    }

    void FPropertyTable::Bind(void* InObject, CStruct* StructType, void* InDefaultObject)
    {
        Object = InObject;
        Struct = StructType;
        DefaultObject = InDefaultObject;
        Callbacks.Type = StructType;
        Callbacks.Instance = InObject;
    }

    void FPropertyTable::DropRows()
    {
        Categories.clear();
        Customization = nullptr;
        CustomizationHandle = nullptr;
        bCustomizationEditActive = false;
        bDirty = true;
    }

    void FPropertyTable::EnsureTreeBuilt()
    {
        if (!bDirty)
        {
            return;
        }

        FPropertyCustomizationRegistry* Registry = GEngine->GetDevelopmentToolsUI()->GetPropertyCustomizationRegistry();
        Customization = Struct != nullptr ? Registry->GetPropertyCustomizationForType(Struct->GetName()) : nullptr;
        if (Customization == nullptr)
        {
            RebuildTree();
        }
        bDirty = false;
    }

    void FPropertyTable::RebuildTree()
    {
        Categories.clear();
        if (Struct == nullptr || Object == nullptr)
        {
            return;
        }

        for (FProperty* Property : Struct->GetProperties())
        {
            if (!Property->IsVisible())
            {
                continue;
            }

            const FCStringView Path = Property->HasMetadata("Category") ? Property->GetMetadata("Category") : FCStringView("General");
            PropertyTableInternal::FPropertyRow* Target = nullptr;
            for (const FName& Segment : PropertyTableInternal::SplitCategoryPath(FStringView(Path.c_str(), Path.size())))
            {
                Target = Target == nullptr ? FindOrCreateCategoryRow(Segment) : Target->FindOrCreateCategoryChild(Segment);
            }
            if (Target == nullptr)
            {
                Target = FindOrCreateCategoryRow(FName("General"));
            }
            Target->AddPropertyRow(MakeShared<FPropertyHandle>(Object, DefaultObject, Property));
        }
    }

    void FPropertyTable::ApplyFilterState()
    {
        const bool bFiltering   = PropertyFilter.IsActive();
        const bool bJustActive  = bFiltering && !bWasFiltering;
        const bool bJustCleared = !bFiltering && bWasFiltering;
        bWasFiltering = bFiltering;

        // The tree is rebuilt underneath by array edits and object swaps, so a stale flag would hide a row.
        if (!bFiltering && !bJustCleared)
        {
            return;
        }
        for (const TUniquePtr<PropertyTableInternal::FCategoryPropertyRow>& Row : Categories)
        {
            Row->ApplyFilter(PropertyFilter, bFiltering, bJustActive, bJustCleared);
        }
    }

    void FPropertyTable::DrawCustomization(const FPropertyDrawArgs& Args)
    {
        if (CustomizationHandle == nullptr)
        {
            CustomizationHandle = MakeShared<FPropertyHandle>(Object, nullptr);
        }

        const EPropertyChangeOp Op = Customization->UpdateAndDraw(CustomizationHandle, Args);
        PropertyTableInternal::DispatchPropertyChange(Op, bCustomizationEditActive, Callbacks, nullptr, nullptr, [this]
        {
            Customization->UpdatePropertyValue(CustomizationHandle);
        });
    }

    PropertyTableInternal::FCategoryPropertyRow* FPropertyTable::FindOrCreateCategoryRow(const FName& CategoryName)
    {
        for (const TUniquePtr<PropertyTableInternal::FCategoryPropertyRow>& Row : Categories)
        {
            if (Row->GetCategoryName() == CategoryName)
            {
                return Row.get();
            }
        }

        Categories.push_back(MakeUnique<PropertyTableInternal::FCategoryPropertyRow>(CategoryName, Callbacks));
        return Categories.back().get();
    }

    TSharedPtr<IPropertyTypeCustomization> MakeInlinePropertyCustomization(FProperty* Prop)
    {
        if (Prop->GetType() == EPropertyTypeFlags::Struct)
        {
            FPropertyCustomizationRegistry* Registry = GEngine->GetDevelopmentToolsUI()->GetPropertyCustomizationRegistry();
            return Registry->GetPropertyCustomizationForType(static_cast<FStructProperty*>(Prop)->GetStruct()->GetName());
        }
        return PropertyTableInternal::MakeLeafPropertyCustomization(Prop);
    }

    bool DrawInstancedStructEditor(const char* StrId, FInstancedStruct& Value, CStruct* BaseStruct, FPropertyTable& Table)
    {
        CStruct* Chosen = Value.GetScriptStruct();
        bool bChanged = false;
        if (ImGuiX::StructCombo(StrId, BaseStruct, Chosen))
        {
            Value.InitializeAs(Chosen);
            bChanged = true;
        }

        if (CStruct* Current = Value.GetScriptStruct())
        {
            // Rebinding drops the rows, so only a real change of memory or type does it.
            void* Memory = Value.GetMutableMemory();
            if (Table.GetObject() != Memory || Table.GetType() != Current)
            {
                Table.SetObject(Memory, Current);
            }

            Table.SetShowSearchBar(false);
            Table.DrawTree();
        }
        return bChanged;
    }
}
