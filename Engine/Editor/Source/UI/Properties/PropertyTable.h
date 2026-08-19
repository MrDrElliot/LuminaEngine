#pragma once
#include "Containers/Function.h"
#include "Core/Reflection/PropertyChangedEvent.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Memory/SmartPtr.h"
#include "imgui.h"

namespace Lumina
{
    enum class EPropertyChangeOp : uint8;
    class FPropertyTable;
    class FStructProperty;
    class CStruct;
    struct FInstancedStruct;
    class FPropertyHandle;
    class FArrayProperty;
    class FMapProperty;
    class FMapPropertyRow;
    class FOptionalProperty;
    class FInstancedStructProperty;
    class FProperty;
    struct IPropertyTypeCustomization;
    class CObject;
    class CClass;
}

namespace Lumina
{

    using FPropertyChangedEventFn = TFunction<void(const FPropertyChangedEvent&)>;

    struct FPropertyChangedEventCallbacks
    {
        // Type and Instance travel together: the struct-ops edit hooks are typed on Type, so they must be
        // handed the instance OF that type -- not the changed property's address, which is what a
        // PropertyHandle yields and is a different object entirely for any non-root property.
        CStruct*                Type = nullptr;
        void*                   Instance = nullptr;
        FPropertyChangedEventFn PreChangeCallback;
        FPropertyChangedEventFn PostChangeCallback;
        FPropertyChangedEventFn StartChangeCallback;
        FPropertyChangedEventFn FinishChangeCallback;

        // Optional trailing control (e.g. delete button) in a fixed column at the end of
        // each top-level row; receives that row's property. Null = no trailing column.
        TFunction<void(FProperty*)> RowTrailingControlFn;
        float                       RowTrailingControlWidth = 0.0f;

        // Multi-edit: when set and it returns true for a top-level property, that row renders as a
        // non-editable "(Multiple Values)" because the selected objects disagree on the value. Null = off.
        TFunction<bool(FProperty*)> IsMultiValueFn;
    };
    
    class FPropertyRow
    {
    public:

        FPropertyRow(const FPropertyChangedEventCallbacks& InCallbacks)
            :Callbacks(InCallbacks)
        {}
        
        virtual ~FPropertyRow() = default;
        FPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& Callbacks);

        // This row's own header label width (text + collapse arrow), excluding indent.
        // Overridden per-row-type so the table can size the header column to the widest label.
        virtual float GetMeasuredHeaderTextWidth() const { return 0.0f; }

        // Largest required header width across this row and its expanded children,
        // including indent; auto-sizes the header column each frame.
        float ComputeRequiredHeaderWidth(float Offset) const;

        virtual void DrawHeader(float Offset) { }
        virtual void DrawEditor(bool bReadOnly) { }
        
        virtual bool HasExtraControls() const;
        virtual void DrawExtraControlsSection();
        virtual float GetExtraControlsSectionWidth();

        void DestroyChildren();

        virtual void Update() { }
        void UpdateRow();
        void DrawRow(float Offset, bool bReadOnly);
        virtual void DrawChildren(float ChildOffset, bool bReadOnly);

        // Fired after ResetToDefault(); container rows (array/optional/struct) override
        // to rebuild child rows so the UI reflects the new structure next frame.
        virtual void OnValueResetToDefault() { }
        bool IsReadOnly() const;

        void SetIsArrayElement(bool bTrue) { bArrayElement = bTrue; }
        bool IsArrayElementProperty() const { return bArrayElement; }

        // True only for FCategoryPropertyRow; lets category rows find existing
        // nested-category children when fanning out a `Foo|Bar|Baz` path.
        virtual bool IsCategory() const { return false; }

        // Label this row is matched against by the table's search box. A row with no label (array
        // elements, which are numbered) never matches on its own but can still survive on a
        // descendant's match.
        virtual FStringView GetFilterLabel() const { return FStringView(); }

        // Recomputes visibility for this row and its subtree. A row survives when it matches OR any
        // descendant does -- filtering to a leaf has to keep its ancestors, or the match is unreachable.
        //
        // Also owns the expansion save/restore: while filtering, surviving rows are force-opened so
        // matches are visible without hunting; when the filter clears, every row's manual collapse state
        // is put back exactly as the user left it.
        bool ApplyFilter(const ImGuiTextFilter& Filter, bool bFilterActive, bool bJustActivated, bool bJustCleared);

        bool PassesFilter() const { return bPassesFilter; }

    protected:

        void DispatchChange(EPropertyChangeOp Op);

        // Writes the default into the container, then re-syncs the customization's
        // cached value so DispatchChange's UpdatePropertyValue won't clobber the reset.
        void PerformResetToDefault();

        // Shift+RMB / Shift+LMB on the row's name cell. The clipboard is shared across the whole
        // editor, and a paste is rejected unless the two properties are the same concrete type.
        void CopyPropertyValue();
        void PastePropertyValue();

        
        FPropertyChangedEventCallbacks          Callbacks;
        
        TSharedPtr<IPropertyTypeCustomization>  Customization;
        TSharedPtr<FPropertyHandle>             PropertyHandle;
        FPropertyRow*                           ParentRow = nullptr;
        
        TVector<TUniquePtr<FPropertyRow>>       Children;
        
        EPropertyChangeOp                       ChangeOp = EPropertyChangeOp::None;
        // True between a widget's Started and its Finished. See IsCommitOp in the .cpp.
        bool                                    bEditSessionActive = false;
        bool                                    bArrayElement = false;
        bool                                    bExpanded = true;

        // Read-only state for THIS draw, captured at the top of DrawRow. The container rows consult it
        // through AllowResize()/AllowReorder() so that structural edits (add / insert / remove / clear /
        // reorder) are gated by the same flag that disables the value widgets -- passing bReadOnly down
        // through HasExtraControls/DrawExtraControlsSection would mean changing several virtuals and
        // every override, and missing one silently leaves a live mutate button on a read-only table.
        bool                                    bDrawReadOnly = false;

        // Filter state. bExpandedSaved holds the user's real collapse state for the duration of a
        // filter, since filtering overwrites bExpanded to reveal matches.
        bool                                    bPassesFilter = true;
        bool                                    bExpandedSaved = true;
    };

    class FPropertyPropertyRow : public FPropertyRow
    {
    public:

        FStringView GetFilterLabel() const override;

        FPropertyPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& Callbacks);
        void Update() override;
        void DrawHeader(float Offset) override;
        void DrawEditor(bool bReadOnly) override;
        float GetMeasuredHeaderTextWidth() const override;

        TSharedPtr<FPropertyHandle> GetPropertyHandle() const { return PropertyHandle; }

    };

    class FArrayPropertyRow : public FPropertyRow
    {
    public:

        FArrayPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& Callbacks);
        void Update() override;
        void DrawHeader(float Offset) override;
        void DrawEditor(bool bReadOnly) override;
        float GetMeasuredHeaderTextWidth() const override;
        void DrawChildren(float ChildOffset, bool bReadOnly) override;
        void RebuildChildren();
        bool HasExtraControls() const override;
        float GetExtraControlsSectionWidth() override;
        void DrawExtraControlsSection() override;
        void OnValueResetToDefault() override { RebuildChildren(); }
        TSharedPtr<FPropertyHandle> GetPropertyHandle() const { return PropertyHandle; }

        bool IsInnerFixedHeight() const;

        // Metadata-driven editing restrictions. The "NoResize" meta hides the
        // add/insert/remove/clear actions; "NoReorder" hides move-up/down.
        bool AllowResize() const;
        bool AllowReorder() const;

        // Defer a structural change to next frame's Update: a reallocating mutation
        // invalidates every child's cached ContainerPtr, so reading mid-frame is UB.
        void QueueMutation(TFunction<void()> Mutation);

        FArrayProperty*             ArrayProperty = nullptr;

    private:

        void DrawTruncationRow(float Offset, int HiddenCount);

        TVector<TFunction<void()>>  PendingMutations;
        bool                        bShowAllElements = false;
    };

    // Editor row for THashMap<K,V> / a C# Dictionary: an Add/Clear header with one FMapEntryRow per entry.
    class FMapPropertyRow : public FPropertyRow
    {
    public:

        FMapPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);
        void Update() override;
        void DrawHeader(float Offset) override;
        void DrawEditor(bool bReadOnly) override;
        float GetMeasuredHeaderTextWidth() const override;
        void RebuildChildren();
        bool HasExtraControls() const override;
        float GetExtraControlsSectionWidth() override;
        void DrawExtraControlsSection() override;
        void OnValueResetToDefault() override { RebuildChildren(); }
        TSharedPtr<FPropertyHandle> GetPropertyHandle() const { return PropertyHandle; }

        // The "NoResize" meta hides add/remove/clear.
        bool AllowResize() const;

        // Deferred to next Update: a structural edit reallocates the map, invalidating child slot pointers.
        void QueueMutation(TFunction<void()> Mutation);

        FMapProperty* MapProperty = nullptr;

    private:

        TVector<TFunction<void()>> PendingMutations;
    };

    // One map entry: an editable key in the header column, the value delegated to a composed value row.
    class FMapEntryRow : public FPropertyRow
    {
    public:

        FMapEntryRow(FMapPropertyRow* InMapRow, int64 InIndex, const FPropertyChangedEventCallbacks& InCallbacks);
        ~FMapEntryRow() override;
        void Update() override;
        void DrawHeader(float Offset) override;
        void DrawEditor(bool bReadOnly) override;
        float GetMeasuredHeaderTextWidth() const override;
        bool HasExtraControls() const override { return true; }
        float GetExtraControlsSectionWidth() override { return 22.0f; }
        void DrawExtraControlsSection() override;

    private:

        // Copy the live key slot into PreviousKey; the last unique key to snap back to on a duplicate edit.
        void CaptureKeySnapshot();

        // True if this entry's key now equals another entry's key (which would silently collapse on reload).
        bool KeyDuplicatesAnotherEntry() const;

        FMapPropertyRow*                        MapRow = nullptr;
        FMapProperty*                           Map = nullptr;          // == MapRow->MapProperty, cached for dtor-safe key destruct
        void*                                   MapContainer = nullptr; // map instance, stable for its lifetime
        void*                                   PreviousKey = nullptr;  // owned scratch: last unique key value
        int64                                   EntryIndex = 0;
        TSharedPtr<FPropertyHandle>             KeyHandle;
        TSharedPtr<IPropertyTypeCustomization>  KeyCustomization;
        TUniquePtr<FPropertyRow>                ValueRow;
        EPropertyChangeOp                       KeyChangeOp = EPropertyChangeOp::None;
        bool                                    bKeyEditSessionActive = false;
    };

    class FStructPropertyRow : public FPropertyRow
    {
    public:

        FStructPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);
        ~FStructPropertyRow() override = default;

        void Update() override;
        void DrawHeader(float Offset) override;
        void DrawEditor(bool bReadOnly) override;
        float GetMeasuredHeaderTextWidth() const override;
        void OnValueResetToDefault() override { RebuildChildren(); }

        void RebuildChildren();

    private:

        FStructProperty*            StructProperty = nullptr;
        TUniquePtr<FPropertyTable>  PropertyTable;
    };

    // Editor row for TInstancedStruct<T>. A type-picker combo (T or derived) plus an inline nested
    // table editing the chosen instance.
    class FInstancedStructPropertyRow : public FPropertyRow
    {
    public:

        FInstancedStructPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);
        ~FInstancedStructPropertyRow() override = default;

        void Update() override;
        void DrawHeader(float Offset) override;
        void DrawEditor(bool bReadOnly) override;
        float GetMeasuredHeaderTextWidth() const override;
        void OnValueResetToDefault() override { RebuildChildren(); }

        // Rebuilds the nested table over the currently-stored instance (after a type change or reset).
        void RebuildChildren();

    private:

        FInstancedStructProperty*   InstancedStructProperty = nullptr;
        CStruct*                    CachedType = nullptr;
        TUniquePtr<FPropertyTable>  PropertyTable;
    };

    // Editor row for TOptional<T>: a "Set" checkbox; when engaged, the inner T
    // renders as a child row using whichever PropertyRow fits T's type.
    class FOptionalPropertyRow : public FPropertyRow
    {
    public:

        FOptionalPropertyRow(const TSharedPtr<FPropertyHandle>& InPropHandle, FPropertyRow* InParentRow, const FPropertyChangedEventCallbacks& InCallbacks);

        void Update() override;
        void DrawHeader(float Offset) override;
        void DrawEditor(bool bReadOnly) override;
        float GetMeasuredHeaderTextWidth() const override;
        void OnValueResetToDefault() override;

        // Rebuilds the 0-or-1 child row to match the optional's engaged state, after
        // a checkbox toggle or an external mutation flipping it underneath us.
        void RebuildChildren();

        TSharedPtr<FPropertyHandle> GetPropertyHandle() const { return PropertyHandle; }

    private:

        FOptionalProperty*  OptionalProperty = nullptr;
        bool                bWasEngaged = false;
    };
    
    class FCategoryPropertyRow : public FPropertyRow
    {
    public:

        FCategoryPropertyRow(void* InObj, const FName& InCategory, const FPropertyChangedEventCallbacks& InCallbacks);

        void AddProperty(const TSharedPtr<FPropertyHandle>& InPropHandle);

        // Existing child category row by name, else a new one appended to Children.
        // Used by RebuildTree when expanding an `Outer|Inner` path one segment at a time.
        FCategoryPropertyRow* FindOrCreateChildCategory(const FName& InCategory);

        FName GetCategoryName() const { return Category; }
        FStringView GetFilterLabel() const override;
        bool IsCategory() const override { return true; }

        void DrawHeader(float Offset) override;
        float GetMeasuredHeaderTextWidth() const override;

    private:

        void* OwnerObject = nullptr;
        FName Category;
    };
    
    class EDITOR_API FPropertyTable
    {
        friend class FStructPropertyRow;
        friend class FInstancedStructPropertyRow;

    public:

        FPropertyTable() = default;
        ~FPropertyTable() = default;
        FPropertyTable(void* InObject, CStruct* InType);

        // Derives Type + DefaultObject from the CObject's class CDO; enables the
        // "modified" indicator and reset-to-default for CObject-rooted details panels.
        explicit FPropertyTable(CObject* InObject);

        // Explicit default-object form, for callers that have a hand-built
        // default to compare against (e.g. nested struct rebuild paths).
        FPropertyTable(void* InObject, CStruct* InType, void* InDefaultObject);

        FPropertyTable(const FPropertyTable&) = delete;
        FPropertyTable(FPropertyTable&&) = delete;
        
        bool operator = (const FPropertyTable&) const = delete;
        bool operator = (FPropertyTable&&) = delete;

        void MarkDirty();
        void DrawTree(bool bReadOnly = false);

        // Search box filtering the tree by property/category name. On by default so every tool's details
        // panel gets one; nested tables (a struct or instanced-struct row hosting its own table) turn it
        // off, since a search box per nested row is noise rather than a feature.
        void SetShowSearchBar(bool bShow) { bShowSearchBar = bShow; }

        // Clears the search text and restores every row's pre-filter collapse state.
        void ClearSearch();

        // Drives the filter from outside, for a panel that owns ONE search box spanning several tables
        // (the scene editor's component list). Pair with SetShowSearchBar(false) so the panel's box is the
        // only one on screen.
        void SetSearchText(FStringView Text);

        // Builds the tree if needed, applies the current filter, and reports whether anything survived.
        // Lets a caller skip drawing a section header for a table that would come back empty -- a list of
        // headers with nothing under them is worse than hiding them. Always true for a table drawn by a
        // type customization, which has no row tree to filter.
        bool PrepareAndTestFilter();

        CStruct* GetType() const { return Struct; }
        void* GetObject() const { return Object; }
        void* GetDefaultObject() const { return DefaultObject; }

        void SetObject(void* InObject, CStruct* StructType);
        void SetObject(void* InObject, CStruct* StructType, void* InDefaultObject);
        void SetPreEditCallback(const FPropertyChangedEventFn& Callback);
        void SetPostEditCallback(const FPropertyChangedEventFn& Callback);
        void SetStartEditCallback(const FPropertyChangedEventFn& Callback);
        void SetFinishEditCallback(const FPropertyChangedEventFn& Callback);

        FCategoryPropertyRow* FindOrCreateCategoryRow(const FName& CategoryName);

        FPropertyChangedEventCallbacks ChangeEventCallbacks;
        bool                           bObjectEditSessionActive = false;

        ImGuiTextFilter                PropertyFilter;
        bool                           bShowSearchBar = true;
        // Edge detection: the save/restore of per-row expansion has to happen exactly once on each
        // transition, not every frame the filter happens to be active.
        bool                           bWasFiltering  = false;
        
    protected:
        
        void RebuildTree();

        // Tree build + filter application, split out of DrawTree so PrepareAndTestFilter can run them
        // before the caller has committed to drawing anything.
        void EnsureTreeBuilt();
        void ApplyFilterState();

    private:
        
        bool                                                bDirty = true;
        TSharedPtr<IPropertyTypeCustomization>              Customization;
        TSharedPtr<FPropertyHandle>                         PropertyHandle;
        CStruct*                                            Struct = nullptr;
        void*                                               Object = nullptr;
        void*                                               DefaultObject = nullptr;
        THashMap<FName, TUniquePtr<FCategoryPropertyRow>>   CategoryMap;
        
    };

    // Type picker plus the instance's properties inline, for an FInstancedStruct edited outside a property row.
    bool DrawInstancedStructEditor(const char* StrId, FInstancedStruct& Value, CStruct* BaseStruct, FPropertyTable& Table);
}
