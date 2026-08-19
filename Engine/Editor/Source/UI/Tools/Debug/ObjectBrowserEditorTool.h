#pragma once

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectFlags.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "UI/Tools/EditorTool.h"
#include "imgui.h"

namespace Lumina
{
    class CObject;
    class CObjectBase;
    class FPropertyTable;

    /** Which objects the browser is allowed to list. Class defaults are one-per-class and make up the bulk
     *  of a running editor's object array while almost never being what you are looking for, so they are
     *  opt-in rather than opt-out. */
    struct FObjectBrowserFilter
    {
        bool bShowDefaults       = false;   // OF_DefaultObject
        bool bShowTransient      = true;
        bool bShowPendingDestroy = true;    // OF_MarkedDestroy -- the interesting ones when chasing a leak
        bool bAssetsOnly         = false;
        bool bRootedOnly         = false;   // OF_Rooted: what is holding the graph alive
    };

    /** One row, resolved ONCE per snapshot.
     *
     *  Everything the table filters, sorts or prints lives here as a value. The previous browser called
     *  GetName().ToString() and GetClass()->GetName().ToString() per object PER FRAME, across two full
     *  object-array walks, and sorted with a comparator that allocated two strings per comparison. That, not
     *  the drawing, is what made it unusable at a real object count.
     */
    struct FObjectBrowserRow
    {
        TWeakObjectPtr<CObject> Object;     // weak: listing an object must not keep it alive
        FName        Name;
        FName        ClassName;
        FName        PackageName;
        FString      FlagsText;             // precomputed: decoding flags builds a string
        EObjectFlags Flags = OF_None;
        int32        StrongRefs = 0;
        int32        WeakRefs = 0;
        bool         bIsAsset = false;
    };

    /**
     * Lists live CObjects: what exists, what class it is, which package owns it, and what still references it.
     *
     * Built around a SNAPSHOT rather than a live walk. The object array is iterated only when the snapshot is
     * (re)taken -- manually, or on an interval while auto-refresh is on -- so the per-frame cost is a clipped
     * draw over a prebuilt index list and is independent of how many objects exist.
     */
    class FObjectBrowserEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FObjectBrowserEditorTool)

        FObjectBrowserEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Object Browser", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_LIST_BOX; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void Update(const FUpdateContext& UpdateContext) override;

    private:

        void DrawWindow(bool bIsFocused);
        void DrawToolbar();
        void DrawTable();
        void DrawDetailsPanel();

        /** Walks GObjectArray once and rebuilds Rows. The only place that touches the object array. */
        void TakeSnapshot();

        /** Rebuilds VisibleRows from Rows against the current filters, then sorts. Neither runs per frame. */
        void RebuildVisibleRows();
        void ApplySort();

        /** The selected object, or null once it has been destroyed. */
        CObject* ResolveSelection() const;

        TVector<FObjectBrowserRow> Rows;
        TVector<int32>             VisibleRows;   // indices into Rows

        FObjectBrowserFilter       Filter;
        ImGuiTextFilter            NameFilter;
        ImGuiTextFilter            ClassFilter;

        // WEAK, deliberately. A strong ref would keep alive the very object you opened this tool to watch
        // die, and a raw pointer would dangle the moment it did -- the handle resolves to null instead.
        // Never an index into VisibleRows: that vector is rebuilt by every filter keystroke.
        TWeakObjectPtr<CObject>    SelectedObject;

        TUniquePtr<FPropertyTable> DetailsTable;
        CObject*                   DetailsBoundObject = nullptr;   // compared only; never dereferenced

        bool    bAutoRefresh = true;
        float   RefreshInterval = 1.0f;
        float   TimeSinceRefresh = 0.0f;
        bool    bSnapshotDirty = true;
        bool    bVisibleRowsDirty = true;

        int32   SortColumn = 0;
        bool    bSortAscending = true;

        // Computed during the snapshot, so the header cannot lie (the previous one printed two statics that
        // were never assigned and always read 0).
        int32   TotalAlive = 0;
        int32   TotalSlots = 0;
    };
}
