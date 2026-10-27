#include "PropertyTable.h"

#include <cstdlib>

#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Memory/Memory.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Customizations/CoreTypeCustomization.h"
#include "Customizations/EntityPropertyCustomization.h"
#include "Tools/UI/DevelopmentToolUI.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Core/Templates/IntegerCompare.h"

namespace Lumina
{
    static constexpr int        ComplexArrayDisplayLimit = 32;
    static constexpr int32      MaxLiveElementRows = 1024;
    static constexpr int32      ElementRowMargin = 64;
    static constexpr float      ChildIndentStep = 14.0f;
    static constexpr uint32     ArrayControlSeed = 428768833;
    static constexpr float      ResetColumnWidth = 22.0f;
    static constexpr float      MaxHeaderColumnShare = 0.45f;
    static ImU32 ModifiedMarkerColor() { return EditorColors::U32(EditorColors::Warning()); }

    // Inside a session only Finished commits, since a lifecycle-free widget sends a lone Updated.
    static bool IsCommitOp(EPropertyChangeOp Op, bool bSessionActive)
    {
        return Op == EPropertyChangeOp::Finished
            || (Op == EPropertyChangeOp::Updated && !bSessionActive);
    }

    // For a session that is Started, since by the commit the interactive writes have already landed.
    static bool IsPreEditOp(EPropertyChangeOp Op, bool bSessionActive)
    {
        return Op == EPropertyChangeOp::Started
            || (Op == EPropertyChangeOp::Updated && !bSessionActive);
    }

    // Byte offset of an edited value inside its outer instance; -1 when it sits in a separate heap block.
    static int64 ValueOffsetInOuter(const void* ValuePtr, const void* OuterInstance, const CStruct* OuterType)
    {
        if (ValuePtr == nullptr || OuterInstance == nullptr || OuterType == nullptr)
        {
            return -1;
        }

        const int64 Offset = static_cast<const uint8*>(ValuePtr) - static_cast<const uint8*>(OuterInstance);
        return (Offset >= 0 && Offset < (int64)OuterType->GetSize()) ? Offset : -1;
    }

    static ImU32 CategoryBgColor()     { return EditorColors::U32(EditorColors::RowBg()); }

    // Caller passes explicit Y bounds; the table row's bottom isn't queryable until after row finalization.
    static void DrawModifiedMarker(float RowTopY, float RowBottomY)
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

    static bool IsFixedHeightPropertyType(EPropertyTypeFlags Type)
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

    static TUniquePtr<FPropertyRow> CreatePropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
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
            return MakeUnique<FPropertyPropertyRow>(InPropHandle, InParentRow, InCallbacks);
        }
    }

    // "Outer|Inner" as one name per nesting level, skipping empty segments.
    static TVector<FName> SplitCategoryPath(FStringView Path)
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

    static FStringView TrimWhitespace(FStringView In)
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

    static bool IsIntegerPropertyType(EPropertyTypeFlags Type)
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

    static bool TryReadConditionInteger(FProperty* Property, void* ContainerPtr, int64& OutValue)
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
    static bool TryResolveConditionLiteral(FProperty* Property, const FString& Literal, int64& OutValue)
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
                const FString Name = Entry.first.ToString();
                const size_t Scope = Name.rfind("::");
                const bool bMatches = Name == Literal
                    || (Scope != FString::npos && Name.compare(Scope + 2, FString::npos, Literal) == 0);
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

        char* End = nullptr;
        const long long Parsed = strtoll(Literal.c_str(), &End, 10);
        if (End == Literal.c_str())
        {
            return false;
        }
        OutValue = static_cast<int64>(Parsed);
        return true;
    }

    static bool EvaluateConditionTerm(const FPropertyEditCondition::FTerm& Term, void* ContainerPtr)
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
            int64 Expected = 0;
            if (TryReadConditionInteger(Term.Property, ContainerPtr, Value)
                && TryResolveConditionLiteral(Term.Property, Term.Literal, Expected))
            {
                bResult = Term.Operator == FPropertyEditCondition::EOperator::Equals ? Value == Expected : Value != Expected;
            }
        }

        return Term.bNegated ? !bResult : bResult;
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
            const size_t Equals    = Text.find("==");
            const size_t NotEquals = Text.find("!=");
            if (Equals != FStringView::npos || NotEquals != FStringView::npos)
            {
                const size_t Sep = Equals < NotEquals ? Equals : NotEquals;
                Term.Operator = Sep == Equals ? EOperator::Equals : EOperator::NotEquals;
                NameView = Text.substr(0, Sep);

                FStringView LiteralView = TrimWhitespace(Text.substr(Sep + 2));
                if (LiteralView.size() >= 2 && LiteralView.front() == '"' && LiteralView.back() == '"')
                {
                    LiteralView = LiteralView.substr(1, LiteralView.size() - 2);
                }
                Term.Literal.assign(LiteralView.data(), LiteralView.size());
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

            Terms.push_back(std::move(Term));
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

    FCategoryPropertyRow* FPropertyRow::FindOrCreateCategoryChild(const FName& InCategory, void* OwnerObject)
    {
        for (const TUniquePtr<FPropertyRow>& Child : Children)
        {
            if (Child->IsCategory())
            {
                FCategoryPropertyRow* AsCategory = static_cast<FCategoryPropertyRow*>(Child.get());
                if (AsCategory->GetCategoryName() == InCategory)
                {
                    return AsCategory;
                }
            }
        }

        TUniquePtr<FCategoryPropertyRow> NewRow = MakeUnique<FCategoryPropertyRow>(OwnerObject, InCategory, Callbacks);
        NewRow->ParentRow = this;
        FCategoryPropertyRow* RawPtr = NewRow.get();
        Children.emplace_back(Move(NewRow));
        return RawPtr;
    }

    void FPropertyRow::BuildMemberRows(CStruct* Type, void* Instance, void* DefaultInstance)
    {
        DestroyChildren();
        if (Type == nullptr || Instance == nullptr)
        {
            return;
        }

        for (FProperty* Member : Type->GetProperties())
        {
            if (!Member->IsVisible())
            {
                continue;
            }

            FPropertyRow* Target = this;
            if (Member->HasMetadata("Category"))
            {
                const FCStringView Path = Member->GetMetadata("Category");
                for (const FName& Segment : SplitCategoryPath(FStringView(Path.c_str(), Path.size())))
                {
                    Target = Target->FindOrCreateCategoryChild(Segment, Instance);
                }
            }

            TSharedPtr<FPropertyHandle> Handle = MakeShared<FPropertyHandle>(Instance, DefaultInstance, Member);
            Target->Children.push_back(CreatePropertyRow(Handle, Target, Target->Callbacks));
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
        if (IsArrayElementProperty())
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

    namespace
    {
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

            return true;
        }
    }

    void FPropertyRow::CopyPropertyValue()
    {
        void* Value = PropertyHandle != nullptr ? PropertyHandle->GetValuePtr() : nullptr;
        if (Value == nullptr || PropertyHandle->Property == nullptr)
        {
            return;
        }

        if (!ArePropertiesCompatible(PropertyHandle->Property, PropertyHandle->Property))
        {
            ImGuiX::Notifications::NotifyWarning("'{0}' is not a copyable property type.", PropertyHandle->Property->Name);
            return;
        }

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

        // Bracketed like a discrete edit so undo captures the old value and post-edit hooks fire.
        DispatchChange(EPropertyChangeOp::Started);

        {
            FMemoryReader Reader(GPropertyClipboardBytes);
            FObjectProxyArchiver Proxy(Reader, true);
            PropertyHandle->Property->Serialize(Proxy, Value);
        }

        if (Customization)
        {
            Customization->HandleExternalUpdate(PropertyHandle);
        }

        ChangeOp = EPropertyChangeOp::Updated;
        DispatchChange(EPropertyChangeOp::Updated);
        DispatchChange(EPropertyChangeOp::Finished);
        ChangeOp = EPropertyChangeOp::None;

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

        // Re-sync customization cache after reset; stale cache would push the old value back via UpdatePropertyValue.
        if (Customization)
        {
            Customization->HandleExternalUpdate(PropertyHandle);
        }

        OnValueResetToDefault();

        ChangeOp = EPropertyChangeOp::Updated;
        DispatchChange(EPropertyChangeOp::Updated);
        DispatchChange(EPropertyChangeOp::Finished);
        ChangeOp = EPropertyChangeOp::None;
    }

    void FPropertyRow::DispatchChange(EPropertyChangeOp Op)
    {
        if (Op == EPropertyChangeOp::None || PropertyHandle == nullptr || PropertyHandle->Property == nullptr)
        {
            return;
        }

        // Hoisted above the event so it can carry the commit flag; equal to reading bEditSessionActive after the Started transition below.
        const bool bSessionActiveForHooks = bEditSessionActive || (Op == EPropertyChangeOp::Started);
        const bool bFirePreEdit  = IsPreEditOp(Op, bSessionActiveForHooks);
        const bool bFirePostEdit = IsCommitOp(Op, bSessionActiveForHooks);

        const FPropertyChangedEvent Event{Callbacks.Type, PropertyHandle->Property, PropertyHandle->Property->Name,
            bFirePostEdit, ValueOffsetInOuter(PropertyHandle->GetValuePtr(), Callbacks.Instance, Callbacks.Type)};

        if (Op == EPropertyChangeOp::Started && Callbacks.StartChangeCallback)
        {
            Callbacks.StartChangeCallback(Event);
        }
        // An atomic commit has no drag to open a session, so its undo transaction is opened here.
        else if (Op == EPropertyChangeOp::Finished && !bEditSessionActive && Callbacks.StartChangeCallback)
        {
            Callbacks.StartChangeCallback(Event);
        }

        // Null for script-defined structs, which carry no compile-time struct ops.
        FStructOps* Ops = Callbacks.Type ? Callbacks.Type->GetStructOps() : nullptr;

        if (Op == EPropertyChangeOp::Started)
        {
            bEditSessionActive = true;
        }
        if (Op == EPropertyChangeOp::Finished)
        {
            bEditSessionActive = false;
        }

        if (Ops && Ops->HasPreEdit() && Callbacks.Instance && bFirePreEdit)
        {
            Ops->PreEdit(Callbacks.Instance, Event);
        }

        if (Callbacks.PreChangeCallback)
        {
            Callbacks.PreChangeCallback(Event);
        }

        if (Customization)
        {
            Customization->UpdatePropertyValue(PropertyHandle);
        }

        if (Ops && Ops->HasPostEdit() && Callbacks.Instance && bFirePostEdit)
        {
            Ops->PostEdit(Callbacks.Instance, Event);
        }
        
        if (Callbacks.PostChangeCallback)
        {
            Callbacks.PostChangeCallback(Event);
        }

        if (Op == EPropertyChangeOp::Finished && Callbacks.FinishChangeCallback)
        {
            Callbacks.FinishChangeCallback(Event);
        }
    }

    namespace
    {
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
    }

    void FPropertyRow::DrawRow(float Offset, const FPropertyDrawArgs& Args)
    {
        const bool bReadOnly = Args.bReadOnly;

        // First, since HasExtraControls() resolves through AllowResize(), which reads this.
        bDrawReadOnly = bReadOnly;

        ImGui::PushID(this);

        ImGui::TableNextRow();

        const bool bIsCategory = IsCategory();
        const bool bTopLevel = IsTopLevel();
        const bool bMultipleValues = bTopLevel && PropertyHandle && PropertyHandle->Property
            && Callbacks.IsMultiValueFn && Callbacks.IsMultiValueFn(PropertyHandle->Property);

        // Capture row top before drawing for later modified-marker rendering.
        ImGuiTable* CurrentTable = ImGui::GetCurrentTable();
        const float RowTopY = CurrentTable ? CurrentTable->RowPosY1 : 0.0f;

        // Holding the measured height in an empty row keeps the scroll range and every row below exactly where drawing would put them.
        if (CurrentTable != nullptr && LastDrawnHeight > 0.0f && IsRowOutsideView(*CurrentTable, LastDrawnHeight))
        {
            CurrentTable->RowPosY2 = ImMax(CurrentTable->RowPosY2, CurrentTable->RowPosY1 + LastDrawnHeight);
            ImGui::PopID();

            if (bExpanded && !Children.empty() && !bMultipleValues)
            {
                ImGui::BeginDisabled(IsReadOnly());
                DrawChildren(Offset + ChildIndentStep, Args);
                ImGui::EndDisabled();
            }
            return;
        }

        const bool bHasDefault = !bIsCategory && PropertyHandle && PropertyHandle->HasDefault();
        const bool bDiffers = bHasDefault && PropertyHandle->DiffersFromDefault();

        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        DrawHeader(Offset, Args);

        // Hung off the name cell, the row's only stable click target, since the value cell is the widget's.
        const bool bShiftHeld = ImGui::GetIO().KeyShift;
        if (!bIsCategory && PropertyHandle && PropertyHandle->Property && bShiftHeld && ImGui::IsItemHovered())
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                CopyPropertyValue();
            }
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && !bReadOnly && !IsReadOnly() && !bMultipleValues)
            {
                PastePropertyValue();
            }
        }

        // Skipped while Shift is held, so the copy chord above does not also pop the menu.
        const bool bHasProperty = !bIsCategory && PropertyHandle && PropertyHandle->Property;
        const bool bCanExpand = !Children.empty();
        if (!bShiftHeld && (bHasProperty || bCanExpand))
        {
            if (ImGui::BeginPopupContextItem("##PropCtx"))
            {
                if (bHasProperty)
                {
                    ImGui::BeginDisabled(!bDiffers || bReadOnly || IsReadOnly() || bMultipleValues);
                    if (ImGui::MenuItem(LE_ICON_REFRESH " Reset to Default"))
                    {
                        PerformResetToDefault();
                    }
                    ImGui::EndDisabled();
                }
                if (bHasProperty && bCanExpand)
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
        }

        ImGui::TableNextColumn();
        {
            const bool bHasExtras = HasExtraControls();

            // Gated to top-level rows so nested members do not inherit it from shared callbacks.
            const bool bHasTrailing = Callbacks.RowTrailingControlFn
                && PropertyHandle && PropertyHandle->Property && bTopLevel;
            const float TrailingWidth = Callbacks.RowTrailingControlWidth > 0.0f ? Callbacks.RowTrailingControlWidth : 24.0f;

            const int ColumnCount = 2 + (bHasExtras ? 1 : 0) + (bHasTrailing ? 1 : 0);

            constexpr ImGuiTableFlags Flags = ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingFixedFit;
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2, 0));
            if (ImGui::BeginTable("EditorRow", ColumnCount, Flags))
            {
                ImGui::TableSetupColumn("##Editor", ImGuiTableColumnFlags_WidthStretch);
                if (bHasExtras)
                {
                    ImGui::TableSetupColumn("##Extra", ImGuiTableColumnFlags_WidthFixed, GetExtraControlsSectionWidth());
                }
                if (bHasTrailing)
                {
                    ImGui::TableSetupColumn("##Trailing", ImGuiTableColumnFlags_WidthFixed, TrailingWidth);
                }
                ImGui::TableSetupColumn("##Reset", ImGuiTableColumnFlags_WidthFixed, ResetColumnWidth);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (bMultipleValues)
                {
                    ImGui::TextDisabled("(Multiple Values)");
                }
                else
                {
                    DrawEditor(Args);
                }

                if (bHasExtras)
                {
                    ImGui::TableNextColumn();
                    DrawExtraControlsSection();
                }

                if (bHasTrailing)
                {
                    ImGui::TableNextColumn();
                    Callbacks.RowTrailingControlFn(PropertyHandle->Property);
                }

                ImGui::TableNextColumn();
                if (bDiffers && !bReadOnly && !IsReadOnly() && !bMultipleValues)
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
            }
            ImGui::PopStyleVar();
        }

        // Fetched again, since a table first seen inside this row can grow ImGui's table pool and move the one above.
        if (const ImGuiTable* RowTable = ImGui::GetCurrentTable())
        {
            // What TableEndCell will settle the row at, read before the children open rows of their own.
            const float RowBottom = ImMax(RowTable->RowPosY2, ImGui::GetCurrentWindow()->DC.CursorMaxPos.y + RowTable->RowCellPaddingY);
            LastDrawnHeight = RowBottom - RowTable->RowPosY1;
        }

        if (bDiffers)
        {
            const float RowBottomY = ImGui::GetCursorScreenPos().y;
            DrawModifiedMarker(RowTopY, RowBottomY);
        }

        ImGui::PopID();

        // The aggregate stands in for the subtree, which would otherwise show the focus object's values.
        if (bExpanded && !Children.empty() && !bMultipleValues)
        {
            ImGui::BeginDisabled(IsReadOnly());
            DrawChildren(Offset + ChildIndentStep, Args);
            ImGui::EndDisabled();
        }
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

    bool FPropertyRow::ApplyFilter(const ImGuiTextFilter& Filter, bool bFilterActive, bool bJustActivated, bool bJustCleared)
    {
        // Without this a search would silently expand the whole tree and leave it that way.
        if (bJustActivated)
        {
            bExpandedSaved = bExpanded;
        }

        bool bAnyChildPasses = false;
        for (const TUniquePtr<FPropertyRow>& Child : Children)
        {
            // Not short-circuited, since every descendant needs its own flag updated.
            bAnyChildPasses |= Child->ApplyFilter(Filter, bFilterActive, bJustActivated, bJustCleared);
        }

        if (!bFilterActive)
        {
            bPassesFilter = true;
            if (bJustCleared)
            {
                bExpanded = bExpandedSaved;
            }
            return true;
        }

        const FStringView Label = GetFilterLabel();
        const bool bSelfMatches = !Label.empty() && ImGuiX::PassSearchFilter(Filter, Label);

        bPassesFilter = bSelfMatches || bAnyChildPasses;

        // A row that matched on its own name stays as the user left it rather than dumping its subtree.
        if (bAnyChildPasses)
        {
            bExpanded = true;
        }

        return bPassesFilter;
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

    // A clipped label leads its tooltip with the full name, since the cell no longer shows it.
    static void DrawPropertyTooltip(const FProperty* Property, bool bLabelClipped = false)
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
    static void DrawRowTooltip(const FPropertyHandle* Handle, const FProperty* Property, bool bLabelClipped, FStringView Label = FStringView())
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
    static bool DrawHeaderLabel(const char* Label)
    {
        const bool bClipped = ImGui::CalcTextSize(Label).x > ImGui::GetContentRegionAvail().x;
        ImGui::TextAligned(0.0f, -FLT_MIN, "%s", Label);
        return bClipped;
    }

    // The foldout version of DrawHeaderLabel, drawing the text itself over a header whose own label is hidden.
    static bool CollapsingHeaderEllipsis(const char* Label, ImGuiTreeNodeFlags Flags, bool& bOutClipped, const void* StableId = nullptr)
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
        case EPropertyTypeFlags::Enum:   return FEnumPropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::Delegate: return FDelegatePropertyCustomization::MakeInstance();
        case EPropertyTypeFlags::ComponentRef: return FComponentRefPropertyCustomization::MakeInstance();
        default: return nullptr;
        }
    }

    FPropertyPropertyRow::FPropertyPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InPropHandle, InParentRow, InCallbacks)
    {
        Customization = MakeLeafPropertyCustomization(PropertyHandle->Property);
    }

    void FPropertyPropertyRow::Update()
    {
        DispatchChange(ChangeOp);
        ChangeOp = EPropertyChangeOp::None;
    }

    FStringView FPropertyPropertyRow::GetFilterLabel() const
    {
        // Array elements are labeled by index, which nobody searches, but a descendant match still passes.
        if (IsArrayElementProperty() || PropertyHandle == nullptr || PropertyHandle->Property == nullptr)
        {
            return FStringView();
        }
        const FCStringView Display = PropertyHandle->Property->GetPropertyDisplayName();
        return FStringView(Display.c_str(), Display.size());
    }

    void FPropertyPropertyRow::DrawHeader(float Offset, const FPropertyDrawArgs& Args)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();

        bool bClipped = false;
        if (IsArrayElementProperty())
        {
            ImGui::Text("%lld", static_cast<long long>(PropertyHandle->Index));
        }
        else
        {
            bClipped = DrawHeaderLabel(PropertyHandle->Property->GetPropertyDisplayName().c_str());
        }

        DrawRowTooltip(PropertyHandle.get(), PropertyHandle->Property, bClipped);
    }

    void FPropertyPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        ImGui::BeginDisabled(IsReadOnly());

        if (Customization)
        {
            ChangeOp = Customization->UpdateAndDraw(PropertyHandle, Args);
        }
        else
        {
            ImGui::TextUnformatted(LE_ICON_EXCLAMATION "Missing Property Customization");
        }

        ImGui::EndDisabled();
    }

    float FPropertyPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        if (IsArrayElementProperty())
        {
            char Buf[32];
            snprintf(Buf, sizeof(Buf), "%lld", static_cast<long long>(PropertyHandle->Index));
            return ImGui::CalcTextSize(Buf).x;
        }
        return ImGui::CalcTextSize(PropertyHandle->Property->GetPropertyDisplayName().c_str()).x;
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

    namespace
    {
        constexpr float kElementButtonWidth = 18.0f;

        // Disabled rather than hidden when it cannot act, so every element row keeps the same columns.
        bool ElementButton(const char* Icon, bool bEnabled, const char* Tooltip)
        {
            ImGui::BeginDisabled(!bEnabled);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
            const bool bClicked = ImGuiX::FlatButton(Icon, ImVec2(kElementButtonWidth, 24), ArrayControlSeed);
            ImGui::PopStyleVar();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGuiX::TextTooltip_Internal(Tooltip);
            }
            return bClicked;
        }
    }

    float FPropertyRow::GetExtraControlsSectionWidth()
    {
        if (!bArrayElement)
        {
            return 0.0f;
        }

        // Up and down for a reorderable array, delete for a resizable one, then the options menu.
        const FArrayPropertyRow* ArrayRow = static_cast<const FArrayPropertyRow*>(ParentRow);
        const int32 Buttons = 1 + (ArrayRow->AllowReorder() ? 2 : 0) + (ArrayRow->AllowResize() ? 1 : 0);
        return Buttons * kElementButtonWidth + (Buttons - 1) * ImGui::GetStyle().ItemSpacing.x + 4.0f;
    }

    void FPropertyRow::DrawExtraControlsSection()
    {
        FArrayPropertyRow* ArrayRow = static_cast<FArrayPropertyRow*>(ParentRow);
        FArrayProperty* ArrayProperty = ArrayRow->ArrayProperty;
        void* ContainerPtr = ArrayRow->GetPropertyHandle()->GetValuePtr();
        const size_t ArrayNum = ArrayProperty->GetNum(ContainerPtr);
        const int64 Index = PropertyHandle->Index;
        const bool bAllowResize = ArrayRow->AllowResize();
        const bool bAllowReorder = ArrayRow->AllowReorder();
        const bool bCanMoveUp = bAllowReorder && Index > 0;
        const bool bCanMoveDown = bAllowReorder && ArrayNum > 0 && Cmp::Less(Index, ArrayNum - 1);

        auto QueueMove = [ArrayRow, ArrayProperty, ContainerPtr, Index](int64 Target)
        {
            ArrayRow->QueueMutation([ArrayRow, ArrayProperty, ContainerPtr, Index, Target]
            {
                ArrayProperty->Swap(ContainerPtr, Index, Target);
                ArrayRow->SwapElementExpansion(Index, Target);
            });
        };
        auto QueueRemove = [ArrayRow, ArrayProperty, ContainerPtr, Index]()
        {
            ArrayRow->QueueMutation([ArrayRow, ArrayProperty, ContainerPtr, Index]
            {
                ArrayProperty->RemoveAt(ContainerPtr, Index);
                ArrayRow->RemoveElementExpansion(Index);
            });
        };

        if (bAllowReorder)
        {
            if (ElementButton(LE_ICON_ARROW_UP, bCanMoveUp, "Move element up"))
            {
                QueueMove(Index - 1);
            }
            ImGui::SameLine();
            if (ElementButton(LE_ICON_ARROW_DOWN, bCanMoveDown, "Move element down"))
            {
                QueueMove(Index + 1);
            }
            ImGui::SameLine();
        }
        if (bAllowResize)
        {
            if (ElementButton(LE_ICON_TRASH_CAN, true, "Remove element"))
            {
                QueueRemove();
            }
            ImGui::SameLine();
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
        ImGuiX::FlatButton(LE_ICON_DOTS_HORIZONTAL, ImVec2(kElementButtonWidth, 24), ArrayControlSeed);
        ImGui::PopStyleVar();

        if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft))
        {
            if (bAllowResize && ImGui::MenuItem(LE_ICON_PLUS " Insert New Element"))
            {
                ArrayRow->QueueMutation([ArrayProperty, ContainerPtr]
                {
                    ArrayProperty->PushBack(ContainerPtr, nullptr);
                });
            }

            if (bCanMoveUp && ImGui::MenuItem(LE_ICON_ARROW_UP " Move Element Up"))
            {
                QueueMove(Index - 1);
            }

            if (bCanMoveDown && ImGui::MenuItem(LE_ICON_ARROW_DOWN " Move Element Down"))
            {
                QueueMove(Index + 1);
            }

            if (bAllowResize && ImGui::MenuItem(LE_ICON_TRASH_CAN " Remove Element"))
            {
                QueueRemove();
            }

            ImGui::EndPopup();
        }

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Array Element Options");
        }
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
            // Structural edits must notify like value edits; Started fires before mutation so the undo snapshot captures the pre-edit array.
            DispatchChange(EPropertyChangeOp::Started);

            // Live rows hold the newest state, while elements outside the window keep whatever was saved for them.
            ElementStates.resize(Math::Max((int32)ElementStates.size(), GetElementCount()));
            for (int32 i = 0; i < (int32)Children.size(); ++i)
            {
                FRowExpansionState& State = ElementStates[WindowBegin + i];
                State.clear();
                Children[i]->CaptureExpansion(State);
            }

            for (const TFunction<void()>& Mutation : PendingMutations)
            {
                Mutation();
            }
            PendingMutations.clear();
            RebuildChildren();

            DispatchChange(EPropertyChangeOp::Updated);
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

    void FArrayPropertyRow::DrawHeader(float Offset, const FPropertyDrawArgs& Args)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();

        const size_t ElementCount = ArrayProperty->GetNum(GetPropertyHandle()->GetValuePtr());

        ImGui::SetNextItemOpen(bExpanded);
        ImGui::PushStyleColor(ImGuiCol_Header, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
        const ImGuiTreeNodeFlags Flags = ElementCount ? 0 : ImGuiTreeNodeFlags_Leaf;
        bool bClipped = false;
        bExpanded = CollapsingHeaderEllipsis(ArrayProperty->GetPropertyDisplayName().c_str(), Flags, bClipped);
        ImGui::PopStyleColor(3);

        DrawPropertyTooltip(ArrayProperty, bClipped);
    }

    void FArrayPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        const size_t ElementCount = ArrayProperty->GetNum(GetPropertyHandle()->GetValuePtr());
        ImGui::TextColored(EditorColors::TextMuted(), "%llu Elements", static_cast<unsigned long long>(ElementCount));
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

    void FArrayPropertyRow::DrawTruncationRow(float Offset, int HiddenCount)
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
        DestroyChildren();
        WindowBegin = 0;
        bShowAllElements = false;
        SetWindow(0, Math::Min(GetElementCount(), ComplexArrayDisplayLimit));
    }

    int32 FArrayPropertyRow::GetElementCount() const
    {
        return (int32)ArrayProperty->GetNum(GetPropertyHandle()->GetValuePtr());
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

    void FArrayPropertyRow::SwapElementExpansion(int64 A, int64 B)
    {
        if (A >= 0 && B >= 0 && A < (int64)ElementStates.size() && B < (int64)ElementStates.size())
        {
            std::swap(ElementStates[A], ElementStates[B]);
        }
    }

    void FArrayPropertyRow::RemoveElementExpansion(int64 Index)
    {
        if (Index >= 0 && Index < (int64)ElementStates.size())
        {
            ElementStates.erase(ElementStates.begin() + Index);
        }
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

    float FArrayPropertyRow::GetExtraControlsSectionWidth()
    {
        return 18 * 2 + 4;
    }

    void FArrayPropertyRow::DrawExtraControlsSection()
    {
        FArrayProperty* LocalArrayProperty = ArrayProperty;
        void* LocalContainerPtr = PropertyHandle->GetValuePtr();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
        if (ImGuiX::FlatButton(LE_ICON_PLUS, ImVec2(18, 24), ArrayControlSeed))
        {
            QueueMutation([LocalArrayProperty, LocalContainerPtr]
            {
                LocalArrayProperty->PushBack(LocalContainerPtr, nullptr);
            });
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Add array element");
        }

        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
        if (ImGuiX::FlatButton(LE_ICON_TRASH_CAN, ImVec2(18, 24), ArrayControlSeed))
        {
            QueueMutation([this, LocalArrayProperty, LocalContainerPtr]
            {
                LocalArrayProperty->Clear(LocalContainerPtr);
                ClearElementExpansion();
            });
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Remove all array elements");
        }
    }

    // ---- Map property row ----

    FMapPropertyRow::FMapPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InPropHandle, InParentRow, InCallbacks)
        , MapProperty(static_cast<FMapProperty*>(InPropHandle->Property))
    {
        RebuildChildren();
    }

    void FMapPropertyRow::Update()
    {
        ChangeOp = EPropertyChangeOp::None;

        if (!PendingMutations.empty())
        {
            DispatchChange(EPropertyChangeOp::Started);
            for (const TFunction<void()>& Mutation : PendingMutations)
            {
                Mutation();
            }
            PendingMutations.clear();
            RebuildChildren();
            DispatchChange(EPropertyChangeOp::Updated);
            DispatchChange(EPropertyChangeOp::Finished);
        }
    }

    void FMapPropertyRow::QueueMutation(TFunction<void()> Mutation)
    {
        PendingMutations.push_back(Move(Mutation));
    }

    void FMapPropertyRow::DrawHeader(float Offset, const FPropertyDrawArgs& Args)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();

        const size_t Count = MapProperty->GetNum(GetPropertyHandle()->GetValuePtr());

        ImGui::SetNextItemOpen(bExpanded);
        ImGui::PushStyleColor(ImGuiCol_Header, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
        const ImGuiTreeNodeFlags Flags = Count ? 0 : ImGuiTreeNodeFlags_Leaf;
        bool bClipped = false;
        bExpanded = CollapsingHeaderEllipsis(MapProperty->GetPropertyDisplayName().c_str(), Flags, bClipped);
        ImGui::PopStyleColor(3);

        DrawPropertyTooltip(MapProperty, bClipped);
    }

    void FMapPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        const size_t Count = MapProperty->GetNum(GetPropertyHandle()->GetValuePtr());
        ImGui::TextColored(EditorColors::TextMuted(), "%llu Entries", static_cast<unsigned long long>(Count));
    }

    float FMapPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        return ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(MapProperty->GetPropertyDisplayName().c_str()).x;
    }

    void FMapPropertyRow::RebuildChildren()
    {
        DestroyChildren();

        void* Container = GetPropertyHandle()->GetValuePtr();
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

    float FMapPropertyRow::GetExtraControlsSectionWidth()
    {
        return 18 * 2 + 4;
    }

    void FMapPropertyRow::DrawExtraControlsSection()
    {
        FMapProperty* Map = MapProperty;
        void* Container = PropertyHandle->GetValuePtr();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
        if (ImGuiX::FlatButton(LE_ICON_PLUS, ImVec2(18, 24), ArrayControlSeed))
        {
            QueueMutation([Map, Container]
            {
                // Default key + value; a colliding default key is a no-op (Insert is insert-or-assign).
                const uint32 KeySize = Map->GetKeySize();
                void* KeyScratch = Memory::Malloc(KeySize > 0 ? KeySize : 1, 16);
                Map->ConstructKey(Container, KeyScratch);
                Map->Insert(Container, KeyScratch, nullptr);
                Map->DestructKey(Container, KeyScratch);
                Memory::Free(KeyScratch);
            });
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Add map entry");
        }

        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
        if (ImGuiX::FlatButton(LE_ICON_TRASH_CAN, ImVec2(18, 24), ArrayControlSeed))
        {
            QueueMutation([Map, Container] { Map->Clear(Container); });
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Remove all map entries");
        }
    }

    // ---- Map entry row ----

    static TSharedPtr<FPropertyHandle> MakeMapValueHandle(FMapPropertyRow* MapRow, int64 Index)
    {
        FMapProperty* Map = MapRow->MapProperty;
        void* Container = MapRow->GetPropertyHandle()->GetValuePtr();
        void* DefaultContainer = MapRow->GetPropertyHandle()->GetDefaultValuePtr();
        return MakeShared<FPropertyHandle>(Map, Container, DefaultContainer, Map->GetValueProperty(), Index, false);
    }

    FMapEntryRow::FMapEntryRow(FMapPropertyRow* InMapRow, int64 InIndex, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(MakeMapValueHandle(InMapRow, InIndex), InMapRow, InCallbacks)
        , MapRow(InMapRow)
        , Map(InMapRow->MapProperty)
        , EntryIndex(InIndex)
    {
        void* Container = MapRow->GetPropertyHandle()->GetValuePtr();
        void* DefaultContainer = MapRow->GetPropertyHandle()->GetDefaultValuePtr();
        MapContainer = Container;

        KeyHandle = MakeShared<FPropertyHandle>(Map, Container, DefaultContainer, Map->GetKeyProperty(), InIndex, true);
        KeyCustomization = MakeLeafPropertyCustomization(Map->GetKeyProperty());

        // Last unique key, to revert a duplicate edit to.
        const uint32 KeySize = Map->GetKeySize();
        PreviousKey = Memory::Malloc(KeySize > 0 ? KeySize : 1, 16);
        Map->ConstructKey(MapContainer, PreviousKey);
        CaptureKeySnapshot();

        // A value with member rows (a struct, array or map) has nothing to draw inline, so it nests below the key.
        TUniquePtr<FPropertyRow> Row = CreatePropertyRow(PropertyHandle, this, Callbacks);
        if (MapValueDrawsInline(Map->GetValueProperty()))
        {
            ValueRow = Move(Row);
        }
        else
        {
            Children.push_back(Move(Row));
        }
    }

    bool FMapEntryRow::MapValueDrawsInline(FProperty* ValueProperty)
    {
        if (ValueProperty->GetType() == EPropertyTypeFlags::Struct)
        {
            FPropertyCustomizationRegistry* Registry = GEngine->GetDevelopmentToolsUI()->GetPropertyCustomizationRegistry();
            return Registry->GetPropertyCustomizationForType(static_cast<FStructProperty*>(ValueProperty)->GetStruct()->GetName()) != nullptr;
        }
        return MakeLeafPropertyCustomization(ValueProperty) != nullptr;
    }

    FMapEntryRow::~FMapEntryRow()
    {
        if (PreviousKey != nullptr)
        {
            if (Map != nullptr)
            {
                Map->DestructKey(MapContainer, PreviousKey);
            }
            Memory::Free(PreviousKey);
        }
    }

    void FMapEntryRow::CaptureKeySnapshot()
    {
        if (Map == nullptr || PreviousKey == nullptr)
        {
            return;
        }
        if (const void* Slot = KeyHandle->GetValuePtr())
        {
            Map->GetKeyProperty()->CopyCompleteValue(PreviousKey, Slot);
        }
    }

    bool FMapEntryRow::KeyDuplicatesAnotherEntry() const
    {
        if (Map == nullptr || MapContainer == nullptr)
        {
            return false;
        }
        const void* MyKey = KeyHandle->GetValuePtr();
        if (MyKey == nullptr)
        {
            return false;
        }
        FProperty* KeyProp = Map->GetKeyProperty();
        const size_t Count = Map->GetNum(MapContainer);
        for (size_t j = 0; j < Count; ++j)
        {
            if (static_cast<int64>(j) == EntryIndex)
            {
                continue;
            }
            const void* OtherKey = Map->GetKeyAt(MapContainer, j);
            if (OtherKey != nullptr && KeyProp->Identical(MyKey, OtherKey))
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

        if (Map == nullptr)
        {
            return;
        }

        // While idle, track the committed key so a duplicate edit has a value to revert to.
        if (KeyChangeOp == EPropertyChangeOp::None)
        {
            CaptureKeySnapshot();
            return;
        }

        // Keys are edited in place, since entries are addressed by iteration index and reload re-hashes.
        const bool bKeySessionActiveForHooks = bKeyEditSessionActive || (KeyChangeOp == EPropertyChangeOp::Started);
        const bool bFirePreEdit  = IsPreEditOp(KeyChangeOp, bKeySessionActiveForHooks);
        const bool bFirePostEdit = IsCommitOp(KeyChangeOp, bKeySessionActiveForHooks);

        const FPropertyChangedEvent Event{Callbacks.Type, Map, Map->Name,
            bFirePostEdit, ValueOffsetInOuter(MapContainer, Callbacks.Instance, Callbacks.Type)};
        if (KeyChangeOp == EPropertyChangeOp::Started && Callbacks.StartChangeCallback)
        {
            Callbacks.StartChangeCallback(Event);
        }
        
        // Null for script-defined structs; see FPropertyRow::DispatchChange.
        FStructOps* Ops = Callbacks.Type ? Callbacks.Type->GetStructOps() : nullptr;

        if (KeyChangeOp == EPropertyChangeOp::Started)
        {
            bKeyEditSessionActive = true;
        }
        if (KeyChangeOp == EPropertyChangeOp::Finished)
        {
            bKeyEditSessionActive = false;
        }

        if (Ops && Ops->HasPreEdit() && Callbacks.Instance && bFirePreEdit)
        {
            Ops->PreEdit(Callbacks.Instance, Event);
        }

        if (Callbacks.PreChangeCallback)
        {
            Callbacks.PreChangeCallback(Event);
        }

        if (KeyCustomization) { KeyCustomization->UpdatePropertyValue(KeyHandle); }

        // A key duplicating another entry silently collapses on reload; revert it and re-sync the widget.
        if (KeyChangeOp == EPropertyChangeOp::Finished && KeyDuplicatesAnotherEntry())
        {
            if (void* Slot = KeyHandle->GetValuePtr())
            {
                Map->GetKeyProperty()->CopyCompleteValue(Slot, PreviousKey);
            }
            if (KeyCustomization) { KeyCustomization->HandleExternalUpdate(KeyHandle); }
            ImGuiX::Notifications::NotifyWarning("Duplicate map key ignored; keys must be unique.");
        }
        
        if (Ops && Ops->HasPostEdit() && Callbacks.Instance && bFirePostEdit)
        {
            Ops->PostEdit(Callbacks.Instance, Event);
        }

        if (Callbacks.PostChangeCallback)
        {
            Callbacks.PostChangeCallback(Event);
        }
        if (KeyChangeOp == EPropertyChangeOp::Finished && Callbacks.FinishChangeCallback) { Callbacks.FinishChangeCallback(Event); }
        KeyChangeOp = EPropertyChangeOp::None;
    }

    void FMapEntryRow::DrawHeader(float Offset, const FPropertyDrawArgs& Args)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();

        if (KeyCustomization)
        {
            KeyChangeOp = KeyCustomization->UpdateAndDraw(KeyHandle, FPropertyDrawArgs{ IsReadOnly(), Args.Context });
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
        return 120.0f;
    }

    void FMapEntryRow::DrawExtraControlsSection()
    {
        FMapProperty* MapProperty = MapRow->MapProperty;
        void* Container = MapRow->GetPropertyHandle()->GetValuePtr();
        const int64 Index = EntryIndex;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
        if (ImGuiX::FlatButton(LE_ICON_TRASH_CAN, ImVec2(18, 24), ArrayControlSeed))
        {
            MapRow->QueueMutation([MapProperty, Container, Index]
            {
                if (Container != nullptr && static_cast<size_t>(Index) < MapProperty->GetNum(Container))
                {
                    MapProperty->RemoveByKey(Container, MapProperty->GetKeyAt(Container, static_cast<size_t>(Index)));
                }
            });
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Remove entry");
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

    FString FStructPropertyRow::GetHeaderLabel() const
    {
        if (!IsArrayElementProperty())
        {
            return StructProperty->GetPropertyDisplayName().c_str();
        }

        FString Label = "[";
        Label += std::to_string(PropertyHandle->Index).c_str();
        Label += "]";
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

        FString Title;
        if (TitleProperty->GetType() == EPropertyTypeFlags::Name)
        {
            FName Value;
            TitleProperty->GetValue(StructValue, &Value);
            if (!Value.IsNone())
            {
                Title = Value.ToString();
            }
        }
        else if (TitleProperty->GetType() == EPropertyTypeFlags::String)
        {
            TitleProperty->GetValue(StructValue, &Title);
        }

        if (!Title.empty())
        {
            Label += "  ";
            Label += Title;
        }
        return Label;
    }

    void FStructPropertyRow::DrawHeader(float Offset, const FPropertyDrawArgs& Args)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();

        const FString Label = GetHeaderLabel();

        // A customization draws the whole value in one row, so there is nothing for an arrow to fold.
        if (Customization)
        {
            const bool bLabelClipped = DrawHeaderLabel(Label.c_str());
            DrawRowTooltip(PropertyHandle.get(), StructProperty, bLabelClipped, FStringView(Label.c_str(), Label.size()));
            return;
        }

        ImGui::SetNextItemOpen(bExpanded);
        ImGui::PushStyleColor(ImGuiCol_Header, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
        bool bClipped = false;
        bExpanded = CollapsingHeaderEllipsis(Label.c_str(), 0, bClipped, this);
        ImGui::PopStyleColor(3);

        DrawRowTooltip(PropertyHandle.get(), StructProperty, bClipped, FStringView(Label.c_str(), Label.size()));
    }

    void FStructPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        if (!bExpanded && !Customization)
        {
            return;
        }

        ImGui::BeginDisabled(IsReadOnly());

        if (Customization)
        {
            ChangeOp = Customization->UpdateAndDraw(PropertyHandle, Args);
        }

        ImGui::EndDisabled();
    }

    float FStructPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        const FString Label = GetHeaderLabel();
        const float LabelWidth = ImGui::CalcTextSize(Label.c_str()).x;
        return Customization ? LabelWidth : ImGui::GetTreeNodeToLabelSpacing() + LabelWidth;
    }

    void FStructPropertyRow::RebuildChildren()
    {
        // A customization draws the whole value itself, so member rows would show it twice.
        if (Customization)
        {
            return;
        }
        BuildMemberRows(StructProperty->GetStruct(), PropertyHandle->GetValuePtr(), PropertyHandle->GetDefaultValuePtr());
    }

    static const char* InstancedStructLabel(const CStruct* Type)
    {
        return StructIdentity(Type).c_str();
    }

    // Whether Candidate lists Interface among the interfaces its C# type implements.
    static bool ImplementsInterface(const CStruct* Candidate, FStringView Interface)
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
    static CStruct* DrawInstancedStructTypePicker(const char* StrId, CStruct* Base, FStringView Implements, CStruct* Current, bool& bOutChanged)
    {
        bOutChanged = false;

        TVector<CStruct*> Candidates;
        for (TObjectIterator<CStruct> It; It; ++It)
        {
            CStruct* Candidate = *It;
            if (!IsInstancableStructType(Candidate))
            {
                continue;
            }
            if ((Base == nullptr || Candidate->IsChildOf(Base)) && (Implements.empty() || ImplementsInterface(Candidate, Implements)))
            {
                Candidates.push_back(Candidate);
            }
        }

        Algo::Sort(Candidates, [](CStruct* A, CStruct* B)
        {
            return strcmp(InstancedStructLabel(A), InstancedStructLabel(B)) < 0;
        });

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
        // Detect a type change underneath us (undo, gameplay, reset) and rebuild the nested table.
        if (auto* Instance = static_cast<FInstancedStruct*>(PropertyHandle->GetValuePtr());
            Instance && Instance->GetScriptStruct() != CachedType)
        {
            RebuildChildren();
        }

        DispatchChange(ChangeOp);
        ChangeOp = EPropertyChangeOp::None;
    }

    void FInstancedStructPropertyRow::DrawHeader(float Offset, const FPropertyDrawArgs& Args)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();

        ImGui::SetNextItemOpen(bExpanded);
        ImGui::PushStyleColor(ImGuiCol_Header, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
        bool bClipped = false;
        bExpanded = CollapsingHeaderEllipsis(InstancedStructProperty->GetPropertyDisplayName().c_str(), 0, bClipped);
        ImGui::PopStyleColor(3);

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
            // A type swap reallocates the instance, so run it as one transaction and rebuild the table.
            DispatchChange(EPropertyChangeOp::Started);
            Instance->InitializeAs(Picked);
            RebuildChildren();
            DispatchChange(EPropertyChangeOp::Updated);
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
        CStruct* Type = Instance ? Instance->GetScriptStruct() : nullptr;
        CachedType = Type;

        if (Instance == nullptr || Type == nullptr)
        {
            DestroyChildren();
            return;
        }

        // Diff/reset of the instance's own fields compares against that struct's default instance.
        BuildMemberRows(Type, Instance->GetMutableMemory(), Type->GetDefaultInstance());
    }

    FOptionalPropertyRow::FOptionalPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InPropHandle, InParentRow, InCallbacks)
        , OptionalProperty(static_cast<FOptionalProperty*>(InPropHandle->Property))
    {
        bWasEngaged = OptionalProperty->HasValue(GetPropertyHandle()->GetValuePtr());
        RebuildChildren();
    }

    void FOptionalPropertyRow::Update()
    {
        // Detects an external change such as gameplay code or an undo, and rebuilds before drawing.
        const bool bEngagedNow = OptionalProperty->HasValue(GetPropertyHandle()->GetValuePtr());
        if (bEngagedNow != bWasEngaged)
        {
            bWasEngaged = bEngagedNow;
            RebuildChildren();
        }

        DispatchChange(ChangeOp);
        ChangeOp = EPropertyChangeOp::None;
    }

    void FOptionalPropertyRow::DrawHeader(float Offset, const FPropertyDrawArgs& Args)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();
        const bool bClipped = DrawHeaderLabel(OptionalProperty->GetPropertyDisplayName().c_str());
        DrawRowTooltip(PropertyHandle.get(), OptionalProperty, bClipped);
    }

    void FOptionalPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        void* ContainerPtr = GetPropertyHandle()->GetValuePtr();
        bool bEngaged = OptionalProperty->HasValue(ContainerPtr);

        ImGui::BeginDisabled(Args.bReadOnly || IsReadOnly());
        if (ImGui::Checkbox("##OptionalSet", &bEngaged))
        {
            // Engaging default-constructs the payload, so children rebuild now to keep the UI in sync.
            if (bEngaged)
            {
                OptionalProperty->SetValue(ContainerPtr, nullptr);
            }
            else
            {
                OptionalProperty->Reset(ContainerPtr);
            }

            bWasEngaged = bEngaged;
            RebuildChildren();
            ChangeOp = EPropertyChangeOp::Updated;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (bEngaged)
        {
            ImGui::TextDisabled("set");
        }
        else
        {
            ImGui::TextDisabled("(none)");
        }
    }

    float FOptionalPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        return ImGui::CalcTextSize(OptionalProperty->GetPropertyDisplayName().c_str()).x;
    }

    void FOptionalPropertyRow::OnValueResetToDefault()
    {
        bWasEngaged = OptionalProperty->HasValue(GetPropertyHandle()->GetValuePtr());
        RebuildChildren();
    }

    void FOptionalPropertyRow::RebuildChildren()
    {
        DestroyChildren();

        void* ContainerPtr = GetPropertyHandle()->GetValuePtr();
        void* DefaultContainerPtr = GetPropertyHandle()->GetDefaultValuePtr();
        if (!OptionalProperty->HasValue(ContainerPtr))
        {
            return;
        }

        FProperty* Inner = OptionalProperty->GetInternalProperty();
        if (Inner == nullptr)
        {
            return;
        }

        // Pass &T directly as the child container; default payload is only meaningful when the default is also engaged.
        void* DefaultPayload = nullptr;
        if (DefaultContainerPtr != nullptr && OptionalProperty->HasValue(DefaultContainerPtr))
        {
            DefaultPayload = OptionalProperty->GetValue(DefaultContainerPtr);
        }
        TSharedPtr<FPropertyHandle> InnerHandle = MakeShared<FPropertyHandle>(OptionalProperty->GetValue(ContainerPtr), DefaultPayload, Inner);
        Children.push_back(CreatePropertyRow(InnerHandle, this, Callbacks));
    }

    FCategoryPropertyRow::FCategoryPropertyRow(void* InObj, const FName& InCategory, const FPropertyChangedEventCallbacks& InCallbacks)
        : FPropertyRow(InCallbacks)
        , Category(InCategory)
    {
        OwnerObject = InObj;
    }

    void FCategoryPropertyRow::AddProperty(const TSharedPtr<FPropertyHandle>& InPropHandle)
    {
        TUniquePtr<FPropertyRow> NewRow = CreatePropertyRow(InPropHandle, this, Callbacks);
        Children.emplace_back(Move(NewRow));
    }

    FCategoryPropertyRow* FCategoryPropertyRow::FindOrCreateChildCategory(const FName& InCategory)
    {
        return FindOrCreateCategoryChild(InCategory, OwnerObject);
    }

    FStringView FCategoryPropertyRow::GetFilterLabel() const
    {
        return FStringView(Category.c_str());
    }

    void FCategoryPropertyRow::DrawHeader(float Offset, const FPropertyDrawArgs& Args)
    {
        // A darker background reads as a section break independent of the row-bg alternation.
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, CategoryBgColor());
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, CategoryBgColor());

        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();

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

    namespace
    {
        // The type's default instance to diff against, unless the object is that default itself.
        void* DefaultInstanceFor(void* InObject, CStruct* InType)
        {
            void* Default = InType != nullptr && InObject != nullptr ? InType->GetDefaultInstance() : nullptr;
            return Default != InObject ? Default : nullptr;
        }
    }

    FPropertyTable::FPropertyTable(void* InObject, CStruct* InType)
    {
        Bind(InObject, InType, DefaultInstanceFor(InObject, InType));
    }

    FPropertyTable::FPropertyTable(void* InObject, CStruct* InType, void* InDefaultObject)
    {
        Bind(InObject, InType, InDefaultObject);
    }

    FPropertyTable::FPropertyTable(CObject* InObject)
        : FPropertyTable(InObject, InObject != nullptr ? InObject->GetClass() : nullptr)
    {
    }

    void FPropertyTable::RebuildTree()
    {
        CategoryMap.clear();

        if (Struct == nullptr || Object == nullptr)
        {
            return;
        }

        for (FProperty* Current : Struct->GetProperties())
        {
            if (Current->IsVisible())
            {
                FString CategoryPath = "General";
                if (Current->HasMetadata("Category"))
                {
                    CategoryPath = Current->GetMetadata("Category").c_str();
                }
                
                FCategoryPropertyRow* TargetRow = nullptr;
                for (const FName& SegmentName : SplitCategoryPath(FStringView(CategoryPath.data(), CategoryPath.size())))
                {
                    TargetRow = (TargetRow == nullptr)
                        ? FindOrCreateCategoryRow(SegmentName)
                        : TargetRow->FindOrCreateChildCategory(SegmentName);
                }
                
                if (TargetRow == nullptr)
                {
                    TargetRow = FindOrCreateCategoryRow(FName("General"));
                }

                TSharedPtr<FPropertyHandle> Property = MakeShared<FPropertyHandle>(Object, DefaultObject, Current);
                TargetRow->AddProperty(Property);
            }
        }
    }

    void FPropertyTable::MarkDirty()
    {
        bDirty = true;
    }

    void FPropertyTable::DrawTree(bool bReadOnly)
    {
        DrawTree(FPropertyDrawArgs{ bReadOnly });
    }

    void FPropertyTable::DrawTree(const FPropertyDrawArgs& InArgs)
    {
        // A table with no context of its own inherits the caller's, which is what nests a struct row.
        const FPropertyDrawArgs Args{ InArgs.bReadOnly, (Context != nullptr) ? *Context : InArgs.Context };

        EnsureTreeBuilt();

        if (Customization)
        {
            if (PropertyHandle == nullptr)
            {
                PropertyHandle = MakeShared<FPropertyHandle>(Object, nullptr);
            }

            const EPropertyChangeOp ChangeOp = Customization->UpdateAndDraw(PropertyHandle, Args);
            if (ChangeOp != EPropertyChangeOp::None)
            {
                const bool bObjectSessionActiveForHooks = bObjectEditSessionActive || (ChangeOp == EPropertyChangeOp::Started);
                const bool bFirePreEdit  = IsPreEditOp(ChangeOp, bObjectSessionActiveForHooks);
                const bool bFirePostEdit = IsCommitOp(ChangeOp, bObjectSessionActiveForHooks);

                const FPropertyChangedEvent Event{Struct, PropertyHandle->Property,
                    PropertyHandle->Property ? PropertyHandle->Property->Name : FName(), bFirePostEdit,
                    ValueOffsetInOuter(PropertyHandle->GetValuePtr(), ChangeEventCallbacks.Instance, ChangeEventCallbacks.Type)};

                if (ChangeOp == EPropertyChangeOp::Started && ChangeEventCallbacks.StartChangeCallback)
                {
                    ChangeEventCallbacks.StartChangeCallback(Event);
                }
                
                // Null for script-defined structs; see FPropertyRow::DispatchChange.
                FStructOps* Ops = ChangeEventCallbacks.Type ? ChangeEventCallbacks.Type->GetStructOps() : nullptr;

                if (ChangeOp == EPropertyChangeOp::Started)
                {
                    bObjectEditSessionActive = true;
                }
                if (ChangeOp == EPropertyChangeOp::Finished)
                {
                    bObjectEditSessionActive = false;
                }

                if (Ops && Ops->HasPreEdit() && ChangeEventCallbacks.Instance && bFirePreEdit)
                {
                    Ops->PreEdit(ChangeEventCallbacks.Instance, Event);
                }

                if (ChangeEventCallbacks.PreChangeCallback)
                {
                    ChangeEventCallbacks.PreChangeCallback(Event);
                }

                Customization->UpdatePropertyValue(PropertyHandle);

                if (Ops && Ops->HasPostEdit() && ChangeEventCallbacks.Instance && bFirePostEdit)
                {
                    Ops->PostEdit(ChangeEventCallbacks.Instance, Event);
                }
                
                if (ChangeEventCallbacks.PostChangeCallback)
                {
                    ChangeEventCallbacks.PostChangeCallback(Event);
                }

                if (ChangeOp == EPropertyChangeOp::Finished && ChangeEventCallbacks.FinishChangeCallback)
                {
                    ChangeEventCallbacks.FinishChangeCallback(Event);
                }
            }
            return;
        }

        constexpr ImGuiTableFlags Flags =
            ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_NoBordersInBodyUntilResize |
            ImGuiTableFlags_SizingStretchSame |
            ImGuiTableFlags_RowBg;

        // Search runs first, so a filtered-out row cannot size the header column.
        if (bShowSearchBar)
        {
            // PushID(this) only covers rows, so two tables in one window collided on the same widget id.
            ImGui::PushID(this);
            ImGuiX::SearchBar("##PropertySearch", PropertyFilter, LE_ICON_MAGNIFY " Search properties...");
            ImGui::PopID();
        }

        ApplyFilterState();

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 3));
        ImGui::PushStyleColor(ImGuiCol_TableRowBg, EditorColors::U32(EditorColors::PanelBg()));
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, EditorColors::U32(EditorColors::RowBg()));
        ImGui::PushID(this);
        
        float HeaderColumnWidth = 0.0f;
        for (auto& [Name, Row] : CategoryMap)
        {
            if (!Row->PassesFilter())
            {
                continue;
            }
            HeaderColumnWidth = Math::Max(HeaderColumnWidth, Row->ComputeRequiredHeaderWidth(0.0f));
        }
        HeaderColumnWidth += 18.0f;

        // Long or deeply nested labels clip and keep their hover tooltip, rather than squeezing the values out.
        HeaderColumnWidth = Math::Min(HeaderColumnWidth, Math::Max(ImGui::GetContentRegionAvail().x * MaxHeaderColumnShare, 80.0f));

        if (ImGui::BeginTable("GridTable", 2, Flags))
        {
            ImGui::TableSetupColumn("##Header", ImGuiTableColumnFlags_WidthFixed, HeaderColumnWidth);
            ImGui::TableSetupColumn("##Editor", ImGuiTableColumnFlags_WidthStretch);

            for (auto& [Name, Row] : CategoryMap)
            {
                // UpdateRow drains queued array mutations, so skipping it strands an edit made before the search.
                Row->UpdateRow();
                if (Row->PassesFilter())
                {
                    Row->DrawRow(0.0f, Args);
                }
            }

            ImGui::EndTable();
        }

        ImGui::PopID();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }

    void FPropertyTable::UpdateRows()
    {
        if (bDirty || Customization)
        {
            return;
        }
        for (auto& [Name, Row] : CategoryMap)
        {
            Row->UpdateRow();
        }
    }

    void FPropertyTable::OnScriptsReloaded(const Scripting::FScriptReload& Reload)
    {
        // Only an object table can be reinstanced, so a struct table's value pointer is never looked up as one.
        const bool bObjectTable = Struct != nullptr && Struct->IsA<CClass>();
        if (CObject* Replacement = bObjectTable ? Reload.FindReplacement(Object) : nullptr)
        {
            Bind(Replacement, Replacement->GetClass(), DefaultInstanceFor(Replacement, Replacement->GetClass()));
        }
        else if (CObject* ReplacedType = Reload.FindReplacement(Struct))
        {
            Bind(Object, static_cast<CStruct*>(ReplacedType), DefaultObject);
        }

        // Dropped rather than reused, since the properties the old rows point at belong to the previous block.
        CategoryMap.clear();
        Customization = nullptr;
        bDirty = true;
    }

    void FPropertyTable::EnsureTreeBuilt()
    {
        if (!bDirty)
        {
            return;
        }

        FPropertyCustomizationRegistry* Registry = GEngine->GetDevelopmentToolsUI()->GetPropertyCustomizationRegistry();
        Customization = Registry->GetPropertyCustomizationForType(Struct->GetName());

        if (Customization == nullptr)
        {
            RebuildTree();
        }
        bDirty = false;
    }

    void FPropertyTable::ApplyFilterState()
    {
        const bool bFiltering   = PropertyFilter.IsActive();
        const bool bJustActive  = bFiltering && !bWasFiltering;
        const bool bJustCleared = !bFiltering && bWasFiltering;
        bWasFiltering = bFiltering;

        // The tree is rebuilt underneath by array edits and object swaps, so a stale flag would hide a row.
        if (bFiltering || bJustCleared)
        {
            for (auto& [Name, Row] : CategoryMap)
            {
                Row->ApplyFilter(PropertyFilter, bFiltering, bJustActive, bJustCleared);
            }
        }
    }

    void FPropertyTable::SetSearchText(FStringView Text)
    {
        const size_t Capacity = sizeof(PropertyFilter.InputBuf) - 1;
        const size_t Count    = Text.size() < Capacity ? Text.size() : Capacity;
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

        for (auto& [Name, Row] : CategoryMap)
        {
            if (Row->PassesFilter())
            {
                return true;
            }
        }
        return false;
    }

    void FPropertyTable::ClearSearch()
    {
        PropertyFilter.Clear();

        // Restored immediately, so the button press and the restore look like one action.
        if (bWasFiltering)
        {
            for (auto& [Name, Row] : CategoryMap)
            {
                Row->ApplyFilter(PropertyFilter, /*bFilterActive*/ false, /*bJustActivated*/ false, /*bJustCleared*/ true);
            }
            bWasFiltering = false;
        }
    }

    void FPropertyTable::SetObject(void* InObject, CStruct* StructType)
    {
        if (Object != InObject || Struct != StructType)
        {
            SetObject(InObject, StructType, DefaultInstanceFor(InObject, StructType));
        }
    }

    void FPropertyTable::SetObject(void* InObject, CStruct* StructType, void* InDefaultObject)
    {
        // A rebuild drops every row and its expansion state, so repointing at what the table already holds is skipped.
        if (Object == InObject && Struct == StructType && DefaultObject == InDefaultObject)
        {
            return;
        }
        Bind(InObject, StructType, InDefaultObject);
        RebuildTree();
    }

    void FPropertyTable::Bind(void* InObject, CStruct* StructType, void* InDefaultObject)
    {
        Object = InObject;
        Struct = StructType;
        // Kept in lockstep with the object; a stale Type here would type-pun the edit hooks.
        ChangeEventCallbacks.Type = StructType;
        ChangeEventCallbacks.Instance = InObject;
        DefaultObject = InDefaultObject;
    }

    void FPropertyTable::SetPreEditCallback(const FPropertyChangedEventFn& Callback)
    {
        ChangeEventCallbacks.PreChangeCallback = Callback;
    }

    void FPropertyTable::SetPostEditCallback(const FPropertyChangedEventFn& Callback)
    {
        ChangeEventCallbacks.PostChangeCallback = Callback;
    }

    void FPropertyTable::SetStartEditCallback(const FPropertyChangedEventFn& Callback)
    {
        ChangeEventCallbacks.StartChangeCallback = Callback;
    }

    void FPropertyTable::SetFinishEditCallback(const FPropertyChangedEventFn& Callback)
    {
        ChangeEventCallbacks.FinishChangeCallback = Callback;
    }

    FCategoryPropertyRow* FPropertyTable::FindOrCreateCategoryRow(const FName& CategoryName)
    {
        auto It = CategoryMap.find(CategoryName);
        if (It == CategoryMap.end())
        {
            auto NewRow = MakeUnique<FCategoryPropertyRow>(Object, CategoryName, ChangeEventCallbacks);
            It = CategoryMap.emplace(CategoryName, Move(NewRow)).first;
        }
        return It->second.get();
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
            // SetObject rebuilds the row tree, so doing it every frame would discard any edit in progress.
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
