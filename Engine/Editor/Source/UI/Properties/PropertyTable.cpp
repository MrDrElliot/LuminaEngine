#include "PropertyTable.h"

#include <EASTL/algorithm.h>

#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Memory/Memory.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Customizations/CoreTypeCustomization.h"
#include "Customizations/EntityPropertyCustomization.h"
#include "Tools/UI/DevelopmentToolUI.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/EditorColors.h"

namespace Lumina
{
    static constexpr int        ComplexArrayDisplayLimit = 32;
    static constexpr float      ChildIndentStep = 14.0f;
    static constexpr uint32     ArrayControlSeed = 428768833;
    static constexpr float      ResetColumnWidth = 22.0f;
    static ImU32 ModifiedMarkerColor() { return EditorColors::U32(EditorColors::Warning()); }
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

    FPropertyRow::FPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks)
        : Callbacks(InCallbacks)
        , PropertyHandle(InPropHandle)
        , ParentRow(InParentRow)
    {
    }

    void FPropertyRow::DestroyChildren()
    {
        Children.clear();
    }

    void FPropertyRow::UpdateRow()
    {
        Update();

        // Skip collapsed subtrees: children only accumulate ChangeOp when drawn, and huge arrays get expensive.
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
                Width = std::max(Width, Child->ComputeRequiredHeaderWidth(Offset + ChildIndentStep));
            }
        }
        return Width;
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

        const FPropertyChangedEvent Event{Callbacks.Type, PropertyHandle->Property, PropertyHandle->Property->Name};

        if (Op == EPropertyChangeOp::Started && Callbacks.StartChangeCallback)
        {
            Callbacks.StartChangeCallback(Event);
        }

        // Null for script-defined structs, which carry no compile-time struct ops.
        FStructOps* Ops = Callbacks.Type ? Callbacks.Type->GetStructOps() : nullptr;

        if (Ops && Ops->HasPreEdit())
        {
            Ops->PreEdit(PropertyHandle->GetValuePtr(), Event);
        }

        if (Callbacks.PreChangeCallback)
        {
            Callbacks.PreChangeCallback(Event);
        }

        if (Customization)
        {
            Customization->UpdatePropertyValue(PropertyHandle);
        }

        if (Ops && Ops->HasPostEdit())
        {
            Ops->PostEdit(PropertyHandle->GetValuePtr(), Event);
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

    void FPropertyRow::DrawRow(float Offset, bool bReadOnly)
    {
        ImGui::PushID(this);

        ImGui::TableNextRow();

        const bool bIsCategory = IsCategory();
        const bool bHasDefault = !bIsCategory && PropertyHandle && PropertyHandle->HasDefault();
        const bool bDiffers = bHasDefault && PropertyHandle->DiffersFromDefault();

        // Multi-edit: a top-level property whose value disagrees across the selected objects is shown
        // as a non-editable "(Multiple Values)" row. Only top-level rows (direct category children) qualify.
        const bool bTopLevel = ParentRow && ParentRow->IsCategory();
        const bool bMultipleValues = bTopLevel && PropertyHandle && PropertyHandle->Property
            && Callbacks.IsMultiValueFn && Callbacks.IsMultiValueFn(PropertyHandle->Property);

        // Capture row top before drawing for later modified-marker rendering.
        ImGuiTable* CurrentTable = ImGui::GetCurrentTable();
        const float RowTopY = CurrentTable ? CurrentTable->RowPosY1 : 0.0f;

        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        DrawHeader(Offset);

        // Right-click context menu; categories don't get one.
        if (!bIsCategory && PropertyHandle && PropertyHandle->Property)
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

            // Opt-in trailing control (e.g. delete button), gated to top-level rows so
            // nested struct members / array elements don't inherit it from shared callbacks.
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
                    DrawEditor(bReadOnly);
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

        // Multi-value top-level rows hide their children; the aggregate "(Multiple Values)" stands in
        // for the whole subtree (which would otherwise show the focus object's values, misleadingly).
        if (bExpanded && !Children.empty() && !bMultipleValues)
        {
            ImGui::BeginDisabled(IsReadOnly());
            DrawChildren(Offset + ChildIndentStep, bReadOnly);
            ImGui::EndDisabled();
        }
    }

    void FPropertyRow::DrawChildren(float ChildOffset, bool bReadOnly)
    {
        for (const TUniquePtr<FPropertyRow>& Row : Children)
        {
            Row->DrawRow(ChildOffset, bReadOnly);
        }
    }

    bool FPropertyRow::IsReadOnly() const
    {
        return PropertyHandle == nullptr ? false : PropertyHandle->Property->IsReadOnly();
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

    // Leaf-property customization factory (numeric / bool / object / name / string / enum ...). Shared by the
    // leaf property row and the map-entry key/value editors. Returns null for non-leaf types (struct/array/...),
    // which have their own dedicated rows.
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
            // A uint32 tagged PROPERTY(Entity) is an entity reference: draw the picker instead of a raw number.
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

    void FPropertyPropertyRow::DrawHeader(float Offset)
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

    void FPropertyPropertyRow::DrawEditor(bool bReadOnly)
    {
        ImGui::BeginDisabled(IsReadOnly());

        if (Customization)
        {
            ChangeOp = Customization->UpdateAndDraw(PropertyHandle, bReadOnly);
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

            if (bAllowReorder && ArrayNum > 0 && std::cmp_less(Index, ArrayNum - 1) && ImGui::MenuItem(LE_ICON_ARROW_DOWN " Move Element Down"))
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

    void FArrayPropertyRow::DrawHeader(float Offset)
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

    void FArrayPropertyRow::DrawEditor(bool bReadOnly)
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

    void FArrayPropertyRow::DrawChildren(float ChildOffset, bool bReadOnly)
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
                    Children[i]->DrawRow(ChildOffset, bReadOnly);
                }
            }
            return;
        }
        
        const int DisplayCount = bShowAllElements ? ChildCount : std::min(ChildCount, ComplexArrayDisplayLimit);
        for (int i = 0; i < DisplayCount; ++i)
        {
            Children[i]->DrawRow(ChildOffset, bReadOnly);
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
        return ArrayProperty == nullptr || !ArrayProperty->HasMetadata("NoResize");
    }

    bool FArrayPropertyRow::AllowReorder() const
    {
        return ArrayProperty == nullptr || !ArrayProperty->HasMetadata("NoReorder");
    }

    bool FArrayPropertyRow::HasExtraControls() const
    {
        // The array-level extra controls are add-element and clear-all, both
        // resize operations, so a NoResize array hides them entirely.
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

    void FMapPropertyRow::DrawHeader(float Offset)
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

    void FMapPropertyRow::DrawEditor(bool bReadOnly)
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
        return MapProperty == nullptr || !MapProperty->HasMetadata("NoResize");
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

        // Commit the in-flight key edit, then notify the map field. Keys are edited in place (entries are
        // addressed by iteration index, and save/reload re-hashes).
        const FPropertyChangedEvent Event{Callbacks.Type, Map, Map->Name};
        if (KeyChangeOp == EPropertyChangeOp::Started && Callbacks.StartChangeCallback)
        {
            Callbacks.StartChangeCallback(Event);
        }
        
        // Null for script-defined structs; see FPropertyRow::DispatchChange.
        FStructOps* Ops = Callbacks.Type ? Callbacks.Type->GetStructOps() : nullptr;

        if (Ops && Ops->HasPreEdit())
        {
            Ops->PreEdit(PropertyHandle->GetValuePtr(), Event);
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
        
        if (Ops && Ops->HasPostEdit())
        {
            Ops->PostEdit(PropertyHandle->GetValuePtr(), Event);
        }

        if (Callbacks.PostChangeCallback)
        {
            Callbacks.PostChangeCallback(Event);
        }
        if (KeyChangeOp == EPropertyChangeOp::Finished && Callbacks.FinishChangeCallback) { Callbacks.FinishChangeCallback(Event); }
        KeyChangeOp = EPropertyChangeOp::None;
    }

    void FMapEntryRow::DrawHeader(float Offset)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();

        if (KeyCustomization)
        {
            KeyChangeOp = KeyCustomization->UpdateAndDraw(KeyHandle, IsReadOnly());
        }
        else
        {
            // Non-leaf key (e.g. a struct): read-only placeholder, edited via add/remove.
            ImGui::TextDisabled("<key %lld>", static_cast<long long>(EntryIndex));
        }
    }

    void FMapEntryRow::DrawEditor(bool bReadOnly)
    {
        if (ValueRow)
        {
            ValueRow->DrawEditor(bReadOnly);
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

    void FStructPropertyRow::DrawHeader(float Offset)
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

    void FStructPropertyRow::DrawEditor(bool bReadOnly)
    {
        if (!bExpanded)
        {
            return;
        }

        ImGui::BeginDisabled(IsReadOnly());

        if (Customization)
        {
            ChangeOp = Customization->UpdateAndDraw(PropertyHandle, bReadOnly);
        }
        else if (PropertyTable)
        {
            PropertyTable->DrawTree(bReadOnly);
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
        // Forward the owning row's change callbacks so edits to nested-struct members notify
        // (save/undo) exactly like top-level property edits.
        PropertyTable->ChangeEventCallbacks = Callbacks;
        PropertyTable->RebuildTree();
    }

    // Display label for a candidate. The script candidate's C# type name in ScriptTypeName metadata
    // when present, else the struct's own name.
    static const char* InstancedStructLabel(const CStruct* Type)
    {
        if (const FString* ScriptName = Type->Metadata.TryGetMetadata("ScriptTypeName"))
        {
            return ScriptName->c_str();
        }
        return Type->GetName().c_str();
    }

    // Type picker over Base and its derived structs, with a None entry at index 0. Returns the chosen
    // type (or Current when unchanged). ScriptInstanceBase markers are hidden.
    static CStruct* DrawInstancedStructTypePicker(const char* StrId, CStruct* Base, CStruct* Current, bool& bOutChanged)
    {
        bOutChanged = false;

        TVector<CStruct*> Candidates;
        for (TObjectIterator<CStruct> It; It; ++It)
        {
            CStruct* Candidate = *It;
            if (Candidate->Metadata.HasMetadata("ScriptInstanceBase"))
            {
                continue;
            }
            if (Base == nullptr || Candidate->IsChildOf(Base))
            {
                Candidates.push_back(Candidate);
            }
        }

        eastl::sort(Candidates.begin(), Candidates.end(), [](CStruct* A, CStruct* B)
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
            });
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

    void FInstancedStructPropertyRow::DrawHeader(float Offset)
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

    void FInstancedStructPropertyRow::DrawEditor(bool bReadOnly)
    {
        auto* Instance = static_cast<FInstancedStruct*>(PropertyHandle->GetValuePtr());
        if (Instance == nullptr)
        {
            return;
        }

        // Type picker on the header line; visible whether expanded or not so the type is always changeable.
        ImGui::BeginDisabled(bReadOnly || IsReadOnly());

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
            PropertyTable->DrawTree(bReadOnly);
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
        // Detect external mutations of the engaged state (e.g. gameplay code or
        // an undo) and rebuild the child tree to match before drawing.
        const bool bEngagedNow = OptionalProperty->HasValue(GetPropertyHandle()->GetValuePtr());
        if (bEngagedNow != bWasEngaged)
        {
            bWasEngaged = bEngagedNow;
            RebuildChildren();
        }

        DispatchChange(ChangeOp);
        ChangeOp = EPropertyChangeOp::None;
    }

    void FOptionalPropertyRow::DrawHeader(float Offset)
    {
        ImGui::Dummy(ImVec2(Offset, 0));
        ImGui::SameLine();
        ImGui::TextUnformatted(OptionalProperty->GetPropertyDisplayName().c_str());
        DrawPropertyTooltip(OptionalProperty);
    }

    void FOptionalPropertyRow::DrawEditor(bool bReadOnly)
    {
        void* ContainerPtr = GetPropertyHandle()->GetValuePtr();
        bool bEngaged = OptionalProperty->HasValue(ContainerPtr);

        ImGui::BeginDisabled(bReadOnly || IsReadOnly());
        if (ImGui::Checkbox("##OptionalSet", &bEngaged))
        {
            // Engaging default-constructs the payload, disengaging discards it.
            // Children rebuild now to keep the UI in sync (also next frame in Update).
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

    void FCategoryPropertyRow::DrawHeader(float Offset)
    {
        // Categories paint both row cells with a darker background to read as
        // a visual section break independent of the row-bg alternation.
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

            // Don't plumb a default when we are the CDO; avoids a misleading
            // "modified" indicator on the rare CDO-edit flow.
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
                        // Via FStringView: FName(const char*, uint32) is base+NUMBER, so a raw length there yields "Physics_8".
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
        if (bDirty)
        {
            FPropertyCustomizationRegistry* Registry = GEngine->GetDevelopmentToolsUI()->GetPropertyCustomizationRegistry();
            Customization = Registry->GetPropertyCustomizationForType(Struct->GetName());

            if (Customization == nullptr)
            {
                RebuildTree();
            }
            bDirty = false;
        }

        if (Customization)
        {
            if (PropertyHandle == nullptr)
            {
                PropertyHandle = MakeShared<FPropertyHandle>(Object, nullptr);
            }

            const EPropertyChangeOp ChangeOp = Customization->UpdateAndDraw(PropertyHandle, bReadOnly);
            if (ChangeOp != EPropertyChangeOp::None)
            {
                const FPropertyChangedEvent Event{Struct, PropertyHandle->Property,
                    PropertyHandle->Property ? PropertyHandle->Property->Name : FName()};

                if (ChangeOp == EPropertyChangeOp::Started && ChangeEventCallbacks.StartChangeCallback)
                {
                    ChangeEventCallbacks.StartChangeCallback(Event);
                }
                
                // Null for script-defined structs; see FPropertyRow::DispatchChange.
                FStructOps* Ops = ChangeEventCallbacks.Type ? ChangeEventCallbacks.Type->GetStructOps() : nullptr;

                if (Ops && Ops->HasPreEdit())
                {
                    Ops->PreEdit(PropertyHandle->GetValuePtr(), Event);
                }

                if (ChangeEventCallbacks.PreChangeCallback)
                {
                    ChangeEventCallbacks.PreChangeCallback(Event);
                }

                Customization->UpdatePropertyValue(PropertyHandle);

                if (Ops && Ops->HasPostEdit())
                {
                    Ops->PostEdit(PropertyHandle->GetValuePtr(), Event);
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

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 3));
        ImGui::PushStyleColor(ImGuiCol_TableRowBg, EditorColors::U32(EditorColors::PanelBg()));
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, EditorColors::U32(EditorColors::RowBg()));
        ImGui::PushID(this);
        
        float HeaderColumnWidth = 0.0f;
        for (auto& [Name, Row] : CategoryMap)
        {
            HeaderColumnWidth = std::max(HeaderColumnWidth, Row->ComputeRequiredHeaderWidth(0.0f));
        }
        HeaderColumnWidth += 18.0f;

        if (ImGui::BeginTable("GridTable", 2, Flags))
        {
            ImGui::TableSetupColumn("##Header", ImGuiTableColumnFlags_WidthFixed, HeaderColumnWidth);
            ImGui::TableSetupColumn("##Editor", ImGuiTableColumnFlags_WidthStretch);

            for (auto& [Name, Row] : CategoryMap)
            {
                Row->UpdateRow();
                Row->DrawRow(0.0f, bReadOnly);
            }

            ImGui::EndTable();
        }

        ImGui::PopID();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }

    void FPropertyTable::SetObject(void* InObject, CStruct* StructType)
    {
        Object = InObject;
        Struct = StructType;
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
}
