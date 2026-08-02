#pragma once

// Process-global RmlUi backend (shared interfaces/renderer/font/debugger + editor previews);
// the per-world Rml::Context lives on CWorld via FWorldUIContext.

#include "Core/Math/Math.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"
#include "Renderer/RHI.h"

namespace Rml
{
    class Context;
}

namespace Lumina
{
    class CWorld;
    class FRmlUiRenderer;
    struct FWorldUIContext;
    struct SWidgetComponent;
}

namespace Lumina::RmlUi
{
    RUNTIME_API bool            Initialize();
    RUNTIME_API void            Shutdown();

    // Per-world context lifecycle. CWorld owns the returned wrapper; CreateWorldUI
    // builds the Rml::Context, DestroyWorldUI removes it. No external bookkeeping.
    RUNTIME_API TUniquePtr<FWorldUIContext> CreateWorldUI(CWorld* World);
    RUNTIME_API void            DestroyWorldUI(CWorld* World);

    // Game thread: update one world's DOM (called from CWorld::Extract).
    RUNTIME_API void            TickWorldUI(CWorld* World);
    // Render thread: composite one world's UI onto its render target (from the scene's RenderView).
    RUNTIME_API void            RenderWorldUI(const CWorld* World, RHI::FCmdListH CmdList);

    // World-space widgets (SWidgetComponent): Tick lays each document into its RT (game thread, in Extract),
    // Render rasterizes the queued RTs (render thread), Release tears one down from on_destroy.
    RUNTIME_API void            TickWorldWidgets(CWorld* World);
    RUNTIME_API void            RenderWorldWidgets(const CWorld* World, RHI::FCmdListH CmdList);
    RUNTIME_API void            ReleaseWidget(CWorld* World, SWidgetComponent& Component);

    // The world whose context is the active UI target. Set when a world comes
    // up or resumes; cleared when it tears down.
    RUNTIME_API void            SetActiveWorld(CWorld* World);

    // Editor-only preview contexts (not bound to any world); ticked/rendered here.
    RUNTIME_API void            TickEditorContexts();
    RUNTIME_API void            RenderEditorContexts(RHI::FCmdListH CmdList);

    RUNTIME_API Rml::Context*   GetContextForWorld(CWorld* World);

    // True when the cursor is over an interactive element in this world's UI, so
    // editor picking / marquee should yield to it. Reads RmlUi's current hover.
    RUNTIME_API bool            WorldUIWantsMouse(const CWorld* World);

    // RAII: holds the bridge lock and resolves a world's Rml::Context for the call duration; the lock
    // blocks RenderWorldUI from walking the DOM and keeps the context valid against teardown.
    class RUNTIME_API FLockedWorldContext
    {
    public:
        explicit FLockedWorldContext(CWorld* World);
        ~FLockedWorldContext();

        FLockedWorldContext(const FLockedWorldContext&)            = delete;
        FLockedWorldContext& operator=(const FLockedWorldContext&) = delete;

        Rml::Context* Get() const { return Context; }
        Rml::Context* operator->() const { return Context; }
        explicit operator bool() const { return Context != nullptr; }

    private:
        Rml::Context* Context = nullptr;
        bool          bLocked = false;
    };

    // Lay UI out at this size instead of the RT image size; {0,0} reverts. Used by the editor viewport.
    RUNTIME_API void            SetWorldDisplaySize(CWorld* World, const FUIntVector2& Size);

    // Replace a world context's documents with one parsed from in-memory RML; SourceUrl resolves relative includes.
    RUNTIME_API bool            SetWorldInlineDocument(CWorld* World, FStringView Body, FStringView SourceUrl);

    RUNTIME_API FRmlUiRenderer* GetRenderer();

    // Editor preview contexts. Caller owns lifetime; pass Target=nullptr to skip a frame.
    RUNTIME_API Rml::Context*   CreateEditorContext(const char* Name, const FUIntVector2& InitialSize);
    RUNTIME_API void            DestroyEditorContext(Rml::Context* Context);
    RUNTIME_API void            SetEditorContextTarget(Rml::Context* Context, RHI::FTextureH Target, const FUIntVector2& Size);

    /** Editor contexts skip the world auto-DPI heuristic. */
    RUNTIME_API void            SetEditorContextDpiScale(Rml::Context* Context, float Scale);

    /** RGBA8; 0-alpha clear allows the editor to composite its own background. */
    RUNTIME_API void            SetEditorContextClearColor(Rml::Context* Context, const FVector4& Color);

    /** SourceUrl resolves relative includes. Previous document unloads either way; returns false on parse failure. */
    RUNTIME_API bool            ReplaceEditorContextDocument(Rml::Context* Context, FStringView Body, FStringView SourceUrl);
    RUNTIME_API void            ClearEditorContextDocument(Rml::Context* Context);

    //--------------------------------------------------------------------------------------------
    // Editor composition designer: live-DOM slot introspection.
    //
    // After a document is parsed into an editor preview context, EnumerateEditorSlots walks the element
    // tree and reports every addressable container (an element carrying an id) as a "slot" the visual
    // designer can assign a widget into. Offsets/sizes are border-box, in context pixels (the tool maps
    // them to canvas space). Assignment state is NOT reported here: a <template src> directive is consumed
    // at parse time and leaves no DOM element, so the editor reads assignments back from the source text.
    // Takes the bridge lock; game/editor thread only.
    //--------------------------------------------------------------------------------------------

    struct FRmlEditorSlot
    {
        FString  Id;                    // element id -- the assignment anchor
        FString  Tag;                   // element tag name (e.g. "div")
        FVector2 OffsetPx{0.0f, 0.0f};  // absolute border-box offset, context px
        FVector2 SizePx{0.0f, 0.0f};    // border-box size, context px
        int32    Depth = 0;             // tree depth from the root, for indentation
        int32    ChildCount = 0;
    };

    RUNTIME_API void EnumerateEditorSlots(Rml::Context* Context, TVector<FRmlEditorSlot>& OutSlots);

    //--------------------------------------------------------------------------------------------
    // Scripting surface: screen documents + element manipulation + event listeners.
    //
    // These drive a world's own full-screen Rml::Context (the HUD/menu context input is already
    // forwarded to, see FInputViewport) -- distinct from world-space SWidgetComponents and editor
    // previews. Every call takes the bridge StateMutex, so it is safe on the game thread against the
    // render thread's RenderWorldUI. Documents/elements cross as opaque handles (Rml::ElementDocument*
    // / Rml::Element* as void*); they are valid while the owning document is loaded. Backs LuminaSharp's
    // World.UI facade (LuminaSharp_UI_* exports in DotNetGameplay.cpp).
    //--------------------------------------------------------------------------------------------

    // Blittable payload handed to a managed UI event callback; mirrors LuminaSharp's UIEventData.
    struct FUIEventData
    {
        int32 Id;              // Rml::EventId
        int32 Phase;           // Rml::EventPhase (1=Capture, 2=Target, 4=Bubble)
        void* CurrentElement;  // element the listener is attached to
        void* TargetElement;   // deepest element the event originated on
        float MouseX;          // "mouse_x" parameter (context space), 0 if absent
        float MouseY;          // "mouse_y" parameter, 0 if absent
        int32 MouseButton;     // "button" parameter, -1 if absent
        int32 KeyIdentifier;   // "key_identifier" parameter (Rml::Input::KeyIdentifier), 0 if absent
        int32 Modifiers;       // bit0 ctrl, bit1 shift, bit2 alt, bit3 meta
        int32 Pad;             // keep size/alignment deterministic across the boundary (48 bytes)
    };

    using FManagedUIEventThunk = void (*)(void* Context, const FUIEventData* Event);

    // Screen documents (loaded into the world's own context). LoadScreenDocument returns the
    // Rml::ElementDocument* as a handle (hidden until ShowDocument); null on parse/load failure.
    RUNTIME_API void* LoadScreenDocument(CWorld* World, FStringView VirtualPath);
    RUNTIME_API void* LoadScreenDocumentFromMemory(CWorld* World, FStringView Body, FStringView SourceUrl);
    RUNTIME_API void  UnloadScreenDocument(CWorld* World, void* Document);
    RUNTIME_API void  ShowDocument(void* Document, bool bModal, bool bAutoFocus);
    RUNTIME_API void  HideDocument(void* Document);
    RUNTIME_API void  PullDocumentToFront(void* Document);
    RUNTIME_API void* GetDocumentRoot(void* Document);   // the document's body element

    // Element queries.
    RUNTIME_API void* DocumentGetElementById(void* Document, FStringView Id);
    RUNTIME_API void* ElementQuerySelector(void* Element, FStringView Selector);

    // Element mutation.
    RUNTIME_API void    ElementSetInnerRml(void* Element, FStringView Rml);
    RUNTIME_API FString ElementGetInnerRml(void* Element);
    RUNTIME_API void    ElementSetAttribute(void* Element, FStringView Name, FStringView Value);
    RUNTIME_API FString ElementGetAttribute(void* Element, FStringView Name);
    RUNTIME_API void    ElementSetProperty(void* Element, FStringView Name, FStringView Value);   // a CSS property
    RUNTIME_API void    ElementRemoveProperty(void* Element, FStringView Name);
    RUNTIME_API void    ElementSetClass(void* Element, FStringView Class, bool bActive);
    RUNTIME_API bool    ElementIsClassSet(void* Element, FStringView Class);
    RUNTIME_API void    ElementFocus(void* Element);
    RUNTIME_API void    ElementBlur(void* Element);
    RUNTIME_API void    ElementClick(void* Element);

    // Event listeners. Add binds a managed thunk (+ GCHandle context) to one element's event and returns
    // an opaque listener handle (null on failure); Remove detaches + frees it. Listeners are owned by the
    // bridge and reaped when the world's UI is destroyed, so a missed Remove never dangles -- it leaks the
    // managed GCHandle, exactly like a registry signal.
    RUNTIME_API void* AddElementEventListener(CWorld* World, void* Element, FStringView EventType, FManagedUIEventThunk Thunk, void* Context);
    RUNTIME_API void  RemoveElementEventListener(CWorld* World, void* Listener);
    
    // Scalar wire type for a bound variable. Mirrors LuminaSharp's EUIVarType. Numbers cross as double and
    // are coerced to the registered type in the cache (so {{ Health }} formats as int, not "100.000000").
    enum class EUIVarType : int32 { Bool = 0, Int = 1, Float = 2, Double = 3, String = 4 };

    // Native -> managed writeback for two-way bound variables. For String, Number is 0 and (Str,StrLen) is
    // the UTF-8 value (valid only for the call); otherwise Str is null and Number carries the value.
    using FManagedDataSetThunk   = void (*)(void* Context, int32 Field, int32 Type, double Number, const char* Str, int32 StrLen);

    // One argument of a data-event-* call (e.g. the 3 in data-event-click="Select(3)"). UTF-8, valid only
    // for the dispatch call. RmlUi event args arrive as Variants; the bridge stringifies them uniformly.
    struct FUIArg { const char* Ptr; int32 Len; };
    // Native -> managed command dispatch for a data-event-* binding, with its (stringified) arguments.
    using FManagedDataEventThunk = void (*)(void* Context, int32 CommandId, int32 ArgCount, const FUIArg* Args);

    // Create a named data model on the world's context. Context is the managed GCHandle passed back to the
    // thunks. Returns an opaque model handle, or null on failure (no context / duplicate name).
    RUNTIME_API void* CreateDataModel(CWorld* World, FStringView Name, void* Context, FManagedDataSetThunk SetThunk, FManagedDataEventThunk EventThunk);

    // Register a scalar variable (Type is EUIVarType); returns its field id (a dense index), or -1 on failure.
    RUNTIME_API int32 DataModelBindScalar(void* Model, FStringView Name, int32 Type);
    // Register a command callback bound by data-event-*; CommandId is dispatched back through EventThunk.
    RUNTIME_API void  DataModelBindCommand(void* Model, FStringView Name, int32 CommandId);

    // Register a list (array-of-struct) variable for data-for; returns a list field id (its own index space,
    // separate from scalar fields), or -1. Add its columns with DataModelBindListMember, then push rows.
    // Cells are strings (RmlUi formats/compares them via Variant conversion); the list is one-way (display).
    RUNTIME_API int32 DataModelBindList(void* Model, FStringView Name);
    // Add a named member (column) to a list, in RML reachable as {{ item.Name }}; returns its column index.
    RUNTIME_API int32 DataModelBindListMember(void* Model, int32 ListField, FStringView MemberName);
    // Set the row count (clears to that many empty rows); then set every cell, then DataModelListDirty.
    RUNTIME_API void  DataModelListResize(void* Model, int32 ListField, int32 RowCount);
    RUNTIME_API void  DataModelListSetCell(void* Model, int32 ListField, int32 Row, int32 Col, FStringView Value);
    RUNTIME_API void  DataModelListDirty(void* Model, int32 ListField);

    // Push a value into the cache (does NOT mark dirty -- call DataModelDirty after a batch).
    RUNTIME_API void  DataModelSetNumber(void* Model, int32 Field, double Value);
    RUNTIME_API void  DataModelSetString(void* Model, int32 Field, FStringView Value);

    // Mark a variable (or all) dirty so RmlUi refreshes its views on the next context Update.
    RUNTIME_API void  DataModelDirty(void* Model, int32 Field);
    RUNTIME_API void  DataModelDirtyAll(void* Model);

    // Remove the model from its context and free the wrapper. No-op (safe) if the model was already reaped
    // when its world tore down -- the handle is validated against the live set, not dereferenced blindly.
    RUNTIME_API void  DestroyDataModel(void* Model);
}
