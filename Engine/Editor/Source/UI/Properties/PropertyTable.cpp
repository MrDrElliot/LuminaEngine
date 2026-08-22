#include "PropertyTable.h"

#include <cstdlib>

#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Core/Reflection/Type/LuminaTypes.h"
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
    static constexpr float      ChildIndentStep = 14.0f;
    static constexpr uint32     ArrayControlSeed = 428768833;
    static constexpr float      ResetColumnWidth = 22.0f;
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
                TObjectPtr<CObject> Value;
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

        const FString* Expression = Source->TryGetMetadata("EditCondition");
        if (Expression == nullptr || Expression->empty())
        {
            return;
        }

        const FStringView Full(Expression->c_str(), Expression->size());
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

            CStruct** Owner = Containers::GetIf<CStruct*>(&Source->Owner);
            Term.Property = (Owner && *Owner) ? (*Owner)->GetProperty(FName(NameView)) : nullptr;
            if (Term.Property == nullptr || Term.Property == Source)
            {
                Terms.clear();
                return;
            }

            Terms.push_back(std::move(Term));
        }

        bHides = Source->HasMetadata("EditConditionHides");
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

    bool FPropertyRow::IsVisible() const
    {
        if (!EditCondition.IsBound() || !EditCondition.ShouldHide())
        {
            return true;
        }
        return EditCondition.Evaluate(GetEditConditionContainer());
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

    void FPropertyRow::DrawRow(float Offset, const FPropertyDrawArgs& Args)
    {
        const bool bReadOnly = Args.bReadOnly;

        // First, since HasExtraControls() resolves through AllowResize(), which reads this.
        bDrawReadOnly = bReadOnly;

        ImGui::PushID(this);

        ImGui::TableNextRow();

        const bool bIsCategory = IsCategory();
        const bool bHasDefault = !bIsCategory && PropertyHandle && PropertyHandle->HasDefault();
        const bool bDiffers = bHasDefault && PropertyHandle->DiffersFromDefault();
        
        const bool bTopLevel = ParentRow && ParentRow->IsCategory();
        const bool bMultipleValues = bTopLevel && PropertyHandle && PropertyHandle->Property
            && Callbacks.IsMultiValueFn && Callbacks.IsMultiValueFn(PropertyHandle->Property);

        // Capture row top before drawing for later modified-marker rendering.
        ImGuiTable* CurrentTable = ImGui::GetCurrentTable();
        const float RowTopY = CurrentTable ? CurrentTable->RowPosY1 : 0.0f;

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
        if (!bIsCategory && !bShiftHeld && PropertyHandle && PropertyHandle->Property)
        {
            if (ImGui::BeginPopupContextItem("##PropCtx"))
            {
                ImGui::BeginDisabled(!bDiffers || bReadOnly || IsReadOnly() || bMultipleValues);
                if (ImGui::MenuItem(LE_ICON_REFRESH " Reset to Default"))
                {
                    PerformResetToDefault();
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
        }

        ImGui::TableNextColumn();
        {
            const bool bHasExtras = HasExtraControls();

            // Gated to top-level rows so nested members do not inherit it from shared callbacks.
            const bool bHasTrailing = Callbacks.RowTrailingControlFn
                && PropertyHandle && PropertyHandle->Property
                && ParentRow != nullptr && ParentRow->IsCategory();
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

    static void DrawPropertyTooltip(const FProperty* Property)
    {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            return;
        }

        const FString* Tooltip = Property->TryGetMetadata("ToolTip");
        if (Tooltip == nullptr)
        {
            Tooltip = &Property->GetPropertyDisplayName();
        }
        ImGuiX::WrappedTooltip_Internal(*Tooltip);
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
        case EPropertyTypeFlags::UInt32:
            // A uint32 tagged PROPERTY(Entity) draws the picker instead of a raw number.
            if (Prop->HasMetadata("Entity"))
            {
                return FEntityPropertyCustomization::MakeInstance();
            }
            return FNumericPropertyCustomization<uint32, ImGuiDataType_U32>::MakeInstance();
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
        const FString& Display = PropertyHandle->Property->GetPropertyDisplayName();
        return FStringView(Display.c_str(), Display.size());
    }

    void FPropertyPropertyRow::DrawHeader(float Offset, const FPropertyDrawArgs& Args)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();

        if (IsArrayElementProperty())
        {
            ImGui::Text("%lld", static_cast<long long>(PropertyHandle->Index));
        }
        else
        {
            ImGui::TextUnformatted(PropertyHandle->Property->GetPropertyDisplayName().c_str());
        }

        DrawPropertyTooltip(PropertyHandle->Property);
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

    float FPropertyRow::GetExtraControlsSectionWidth()
    {
        return bArrayElement ? 22.0f : 0.0f;
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

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));
        ImGuiX::FlatButton(LE_ICON_DOTS_HORIZONTAL, ImVec2(18, 24), ArrayControlSeed);
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

            if (bAllowReorder && Index > 0 && ImGui::MenuItem(LE_ICON_ARROW_UP " Move Element Up"))
            {
                ArrayRow->QueueMutation([ArrayProperty, ContainerPtr, Index]
                {
                    ArrayProperty->Swap(ContainerPtr, Index, Index - 1);
                });
            }

            if (bAllowReorder && ArrayNum > 0 && Cmp::Less(Index, ArrayNum - 1) && ImGui::MenuItem(LE_ICON_ARROW_DOWN " Move Element Down"))
            {
                ArrayRow->QueueMutation([ArrayProperty, ContainerPtr, Index]
                {
                    ArrayProperty->Swap(ContainerPtr, Index, Index + 1);
                });
            }

            if (bAllowResize && ImGui::MenuItem(LE_ICON_TRASH_CAN " Remove Element"))
            {
                ArrayRow->QueueMutation([ArrayProperty, ContainerPtr, Index]
                {
                    ArrayProperty->RemoveAt(ContainerPtr, Index);
                });
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
        bExpanded = ImGui::CollapsingHeader(ArrayProperty->GetPropertyDisplayName().c_str(), Flags);
        ImGui::PopStyleColor(3);

        DrawPropertyTooltip(ArrayProperty);
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
        const int ChildCount = static_cast<int>(Children.size());
        if (ChildCount == 0)
        {
            return;
        }

        if (IsInnerFixedHeight())
        {
            ImGuiListClipper Clipper;
            Clipper.Begin(ChildCount);
            while (Clipper.Step())
            {
                for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                {
                    Children[i]->DrawRow(ChildOffset, Args);
                }
            }
            return;
        }
        
        const int DisplayCount = bShowAllElements ? ChildCount : Math::Min(ChildCount, ComplexArrayDisplayLimit);
        for (int i = 0; i < DisplayCount; ++i)
        {
            Children[i]->DrawRow(ChildOffset, Args);
        }

        if (DisplayCount < ChildCount)
        {
            DrawTruncationRow(ChildOffset, ChildCount - DisplayCount);
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

        void* ContainerPtr = GetPropertyHandle()->GetValuePtr();
        void* DefaultContainerPtr = GetPropertyHandle()->GetDefaultValuePtr();
        const size_t ElementCount = ArrayProperty->GetNum(ContainerPtr);
        
        bShowAllElements = false;

        Children.reserve(ElementCount);
        FProperty* InnerProperty = ArrayProperty->GetInternalProperty();
        for (size_t i = 0; i < ElementCount; ++i)
        {
            TSharedPtr<FPropertyHandle> ElementPropHandle = MakeShared<FPropertyHandle>(
                ArrayProperty,
                ContainerPtr,
                DefaultContainerPtr,
                InnerProperty,
                static_cast<int64>(i));
            TUniquePtr<FPropertyRow> NewRow = CreatePropertyRow(ElementPropHandle, this, Callbacks);
            NewRow->SetIsArrayElement(true);
            Children.push_back(Move(NewRow));
        }
    }

    bool FArrayPropertyRow::AllowResize() const
    {
        // Disabling only the value widgets left the add and trash buttons live, a bigger edit than refused.
        return !bDrawReadOnly && (ArrayProperty == nullptr || !ArrayProperty->HasMetadata("NoResize"));
    }

    bool FArrayPropertyRow::AllowReorder() const
    {
        return !bDrawReadOnly && (ArrayProperty == nullptr || !ArrayProperty->HasMetadata("NoReorder"));
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
            QueueMutation([LocalArrayProperty, LocalContainerPtr]
            {
                LocalArrayProperty->Clear(LocalContainerPtr);
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
        bExpanded = ImGui::CollapsingHeader(MapProperty->GetPropertyDisplayName().c_str(), Flags);
        ImGui::PopStyleColor(3);

        DrawPropertyTooltip(MapProperty);
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
        return !bDrawReadOnly && (MapProperty == nullptr || !MapProperty->HasMetadata("NoResize"));
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

        ValueRow = CreatePropertyRow(PropertyHandle, this, Callbacks);
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
        
        if (StructProperty->HasMetadata("DefaultCollapsed"))
        {
            bExpanded = false;
        }

        if (!Customization)
        {
            RebuildChildren();
        }
    }

    void FStructPropertyRow::Update()
    {
        DispatchChange(ChangeOp);
        ChangeOp = EPropertyChangeOp::None;
    }

    void FStructPropertyRow::DrawHeader(float Offset, const FPropertyDrawArgs& Args)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();

        ImGui::SetNextItemOpen(bExpanded);
        ImGui::PushStyleColor(ImGuiCol_Header, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
        bExpanded = ImGui::CollapsingHeader(StructProperty->GetPropertyDisplayName().c_str());
        ImGui::PopStyleColor(3);

        DrawPropertyTooltip(StructProperty);
    }

    void FStructPropertyRow::DrawEditor(const FPropertyDrawArgs& Args)
    {
        if (!bExpanded)
        {
            return;
        }

        ImGui::BeginDisabled(IsReadOnly());

        if (Customization)
        {
            ChangeOp = Customization->UpdateAndDraw(PropertyHandle, Args);
        }
        else if (PropertyTable)
        {
            PropertyTable->DrawTree(Args);
        }

        ImGui::EndDisabled();
    }

    float FStructPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        return ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(StructProperty->GetPropertyDisplayName().c_str()).x;
    }

    void FStructPropertyRow::RebuildChildren()
    {
        void* InstancePtr = PropertyHandle->GetValuePtr();
        void* DefaultInstancePtr = PropertyHandle->GetDefaultValuePtr();
        PropertyTable = MakeUnique<FPropertyTable>(InstancePtr, StructProperty->GetStruct(), DefaultInstancePtr);
        // A nested table, since the owning table's search box already covers these properties.
        PropertyTable->SetShowSearchBar(false);
        // Forwarded so a nested-struct member edit notifies save and undo like a top-level one.
        PropertyTable->ChangeEventCallbacks = Callbacks;
        PropertyTable->RebuildTree();
    }

    // The C# type name from ScriptTypeName when present, otherwise the struct's own name.
    static const char* InstancedStructLabel(const CStruct* Type)
    {
        if (const FString* ScriptName = Type->Metadata.TryGetMetadata("ScriptTypeName"))
        {
            return ScriptName->c_str();
        }
        return Type->GetName().c_str();
    }

    // A None entry sits at index 0, and ScriptInstanceBase markers are hidden.
    static CStruct* DrawInstancedStructTypePicker(const char* StrId, CStruct* Base, CStruct* Current, bool& bOutChanged)
    {
        bOutChanged = false;

        TVector<CStruct*> Candidates;
        for (TObjectIterator<CStruct> It; It; ++It)
        {
            CStruct* Candidate = *It;
            if (Candidate->Metadata.HasMetadata("ScriptInstanceBase") || !IsInstancableStructType(Candidate))
            {
                continue;
            }
            if (Base == nullptr || Candidate->IsChildOf(Base))
            {
                Candidates.push_back(Candidate);
            }
        }

        Algo::Sort(Candidates.begin(), Candidates.end(), [](CStruct* A, CStruct* B)
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

        if (Picked != INDEX_NONE)
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
        if (InstancedStructProperty->HasMetadata("DefaultCollapsed"))
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
        bExpanded = ImGui::CollapsingHeader(InstancedStructProperty->GetPropertyDisplayName().c_str());
        ImGui::PopStyleColor(3);

        DrawPropertyTooltip(InstancedStructProperty);
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
        CStruct* Picked = DrawInstancedStructTypePicker("##instancedstructpicker", InstancedStructProperty->GetMetaStruct(), Current, bChanged);

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

        // Inline editor for the chosen instance's own properties.
        if (bExpanded && PropertyTable && Instance->IsValid())
        {
            ImGui::BeginDisabled(IsReadOnly());
            PropertyTable->DrawTree(Args);
            ImGui::EndDisabled();
        }
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
            PropertyTable.reset();
            return;
        }

        // Diff/reset of the instance's own fields compares against that struct's default instance.
        PropertyTable = MakeUnique<FPropertyTable>(Instance->GetMutableMemory(), Type, Type->GetDefaultInstance());
        // A nested table, since the owning table's search box already covers these properties.
        PropertyTable->SetShowSearchBar(false);
        PropertyTable->ChangeEventCallbacks = Callbacks;
        PropertyTable->RebuildTree();
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
        ImGui::TextUnformatted(OptionalProperty->GetPropertyDisplayName().c_str());
        DrawPropertyTooltip(OptionalProperty);
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
        FCategoryPropertyRow* RawPtr = NewRow.get();
        Children.emplace_back(Move(NewRow));
        return RawPtr;
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
        bExpanded = ImGui::CollapsingHeader(Category.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(4);
    }

    float FCategoryPropertyRow::GetMeasuredHeaderTextWidth() const
    {
        return ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(Category.c_str()).x;
    }

    FPropertyTable::FPropertyTable(void* InObject, CStruct* InType)
        : ChangeEventCallbacks()
        , Struct(InType)
        , Object(InObject)
    {
        ChangeEventCallbacks.Type = InType;
        ChangeEventCallbacks.Instance = InObject;
        if (InType != nullptr && InObject != nullptr)
        {
            void* MaybeDefault = InType->GetDefaultInstance();
            if (MaybeDefault != InObject)
            {
                DefaultObject = MaybeDefault;
            }
        }
    }

    FPropertyTable::FPropertyTable(void* InObject, CStruct* InType, void* InDefaultObject)
        : ChangeEventCallbacks()
        , Struct(InType)
        , Object(InObject)
        , DefaultObject(InDefaultObject)
    {
        ChangeEventCallbacks.Type = InType;
        ChangeEventCallbacks.Instance = InObject;
    }

    FPropertyTable::FPropertyTable(CObject* InObject)
        : ChangeEventCallbacks()
        , Object(InObject)
    {
        if (InObject != nullptr)
        {
            CClass* Class = InObject->GetClass();
            Struct = Class;
            ChangeEventCallbacks.Type = Class;
            ChangeEventCallbacks.Instance = InObject;

            // Avoids a misleading modified indicator on the rare CDO-edit flow.
            if (Class != nullptr && !InObject->HasAnyFlag(OF_DefaultObject))
            {
                DefaultObject = Class->GetDefaultObject();
            }
        }
    }

    void FPropertyTable::RebuildTree()
    {
        CategoryMap.clear();

        if (Struct == nullptr || Object == nullptr)
        {
            return;
        }

        FProperty* Current = Struct->LinkedProperty;
        while (Current != nullptr)
        {
            if (Current->IsVisible())
            {
                FString CategoryPath = "General";
                if (Current->Metadata.HasMetadata("Category"))
                {
                    CategoryPath = Current->Metadata.GetMetadata("Category");
                }
                
                FCategoryPropertyRow* TargetRow = nullptr;
                size_t SegmentStart = 0;
                while (SegmentStart <= CategoryPath.length())
                {
                    size_t Sep = CategoryPath.find('|', SegmentStart);
                    if (Sep == FString::npos)
                    {
                        Sep = CategoryPath.length();
                    }

                    if (Sep > SegmentStart)
                    {
                        // Through FStringView, since FName(const char*, uint32) is base plus NUMBER, not a length.
                        FName SegmentName(FStringView(CategoryPath.data() + SegmentStart, Sep - SegmentStart));
                        TargetRow = (TargetRow == nullptr)
                            ? FindOrCreateCategoryRow(SegmentName)
                            : TargetRow->FindOrCreateChildCategory(SegmentName);
                    }

                    if (Sep == CategoryPath.length())
                    {
                        break;
                    }
                    SegmentStart = Sep + 1;
                }
                
                if (TargetRow == nullptr)
                {
                    TargetRow = FindOrCreateCategoryRow(FName("General"));
                }

                TSharedPtr<FPropertyHandle> Property = MakeShared<FPropertyHandle>(Object, DefaultObject, Current);
                TargetRow->AddProperty(Property);
            }

            Current = static_cast<FProperty*>(Current->Next);
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
        Object = InObject;
        Struct = StructType;
        // Kept in lockstep with the object; a stale Type here would type-pun the edit hooks.
        ChangeEventCallbacks.Type = StructType;
        ChangeEventCallbacks.Instance = InObject;
        DefaultObject = nullptr;
        if (StructType != nullptr && InObject != nullptr)
        {
            void* MaybeDefault = StructType->GetDefaultInstance();
            if (MaybeDefault != InObject)
            {
                DefaultObject = MaybeDefault;
            }
        }

        RebuildTree();
    }

    void FPropertyTable::SetObject(void* InObject, CStruct* StructType, void* InDefaultObject)
    {
        Object = InObject;
        Struct = StructType;
        ChangeEventCallbacks.Type = StructType;
        ChangeEventCallbacks.Instance = InObject;
        DefaultObject = InDefaultObject;

        RebuildTree();
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
