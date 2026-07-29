#pragma once
#include "Containers/Function.h"
#include "Core/Reflection/PropertyChangedEvent.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    enum class EPropertyChangeOp : uint8;
    class FPropertyTable;
    class FStructProperty;
    class CStruct;
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

    protected:

        void DispatchChange(EPropertyChangeOp Op);

        // Writes the default into the container, then re-syncs the customization's
        // cached value so DispatchChange's UpdatePropertyValue won't clobber the reset.
        void PerformResetToDefault();

        
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
    };

    class FPropertyPropertyRow : public FPropertyRow
    {
    public:

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
        bool IsCategory() const override { return true; }

        void DrawHeader(float Offset) override;
        float GetMeasuredHeaderTextWidth() const override;

    private:

        void* OwnerObject = nullptr;
        FName Category;
    };
    
    class FPropertyTable
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
        
    protected:
        
        void RebuildTree();

    private:
        
        bool                                                bDirty = true;
        TSharedPtr<IPropertyTypeCustomization>              Customization;
        TSharedPtr<FPropertyHandle>                         PropertyHandle;
        CStruct*                                            Struct = nullptr;
        void*                                               Object = nullptr;
        void*                                               DefaultObject = nullptr;
        THashMap<FName, TUniquePtr<FCategoryPropertyRow>>   CategoryMap;
        
    };
}
