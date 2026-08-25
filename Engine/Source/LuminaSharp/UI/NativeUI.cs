using System;

namespace LuminaSharp;

/// <summary>
/// Native bindings for the World.UI scripting surface (RmlUi). Forward to the flat
/// <c>LuminaSharp_UI_*</c> exports in the Runtime module (DotNetGameplay.cpp), which delegate to the
/// locked <c>RmlUi::</c> bridge. Documents/elements/listeners cross as opaque <see cref="IntPtr"/>
/// handles; the world is its <c>CWorld*</c> as a <see cref="ulong"/>. Game thread only.
/// </summary>
public static unsafe partial class Native
{
    // Documents (loaded into the world's own screen context).
    [NativeCall] public static partial IntPtr UI_LoadDocument(ulong World, string Path);
    [NativeCall] public static partial IntPtr UI_LoadDocumentFromMemory(ulong World, string Body, string Url);
    [NativeCall] public static partial void   UI_UnloadDocument(ulong World, IntPtr Document);
    [NativeCall] public static partial void   UI_ShowDocument(IntPtr Document, int Modal, int AutoFocus);
    [NativeCall] public static partial void   UI_HideDocument(IntPtr Document);
    [NativeCall] public static partial void   UI_PullDocumentToFront(IntPtr Document);
    [NativeCall] public static partial IntPtr UI_GetDocumentRoot(IntPtr Document);

    // Element queries.
    [NativeCall] public static partial IntPtr UI_GetElementById(IntPtr Document, string Id);
    [NativeCall] public static partial IntPtr UI_QuerySelector(IntPtr Element, string Selector);

    // Element mutation.
    [NativeCall] public static partial void   UI_SetInnerRml(IntPtr Element, string Rml);
    [NativeCall] public static partial string UI_GetInnerRml(IntPtr Element);
    [NativeCall] public static partial void   UI_SetAttribute(IntPtr Element, string Name, string Value);
    [NativeCall] public static partial string UI_GetAttribute(IntPtr Element, string Name);
    [NativeCall] public static partial void   UI_SetProperty(IntPtr Element, string Name, string Value);
    [NativeCall] public static partial void   UI_RemoveProperty(IntPtr Element, string Name);
    [NativeCall] public static partial void   UI_SetClass(IntPtr Element, string Class, int Active);
    [NativeCall] public static partial int    UI_IsClassSet(IntPtr Element, string Class);
    [NativeCall] public static partial void   UI_ElementFocus(IntPtr Element);
    [NativeCall] public static partial void   UI_ElementBlur(IntPtr Element);
    [NativeCall] public static partial void   UI_ElementClick(IntPtr Element);

    // Event listeners (managed thunk + GCHandle context, like the registry signals).
    [NativeCall] public static partial IntPtr UI_AddEventListener(ulong World, IntPtr Element, string Type);
    [NativeCall] public static partial void   UI_RemoveEventListener(ulong World, IntPtr Listener);
    [NativeCall] public static partial IntPtr UI_GetEventListenerDelegate(IntPtr Listener);

    // Data binding (MVVM). A named data model on the world context (Model is an opaque FManagedDataModel*).
    // SetThunk/EventThunk are managed function pointers; Context is the GCHandle handed back to them.
    [NativeCall] public static partial IntPtr UI_CreateDataModel(ulong World, string Name, IntPtr Context, IntPtr SetThunk, IntPtr EventThunk);
    [NativeCall] public static partial int    UI_ModelBindScalar(IntPtr Model, string Name, int Type);
    [NativeCall] public static partial void   UI_ModelBindCommand(IntPtr Model, string Name, int CommandId);
    [NativeCall] public static partial void   UI_ModelSetNumber(IntPtr Model, int Field, double Value);
    [NativeCall] public static partial void   UI_ModelSetString(IntPtr Model, int Field, string Value);
    [NativeCall] public static partial void   UI_ModelDirty(IntPtr Model, int Field);
    [NativeCall] public static partial void   UI_ModelDirtyAll(IntPtr Model);
    [NativeCall] public static partial void   UI_DestroyDataModel(IntPtr Model);

    // Lists (data-for): array-of-struct variables; cells pushed as strings on change.
    [NativeCall] public static partial int    UI_ModelBindList(IntPtr Model, string Name);
    [NativeCall] public static partial int    UI_ModelBindListMember(IntPtr Model, int ListField, string Name);
    [NativeCall] public static partial void   UI_ModelListResize(IntPtr Model, int ListField, int RowCount);
    [NativeCall] public static partial void   UI_ModelListSetCell(IntPtr Model, int ListField, int Row, int Col, string Value);
    [NativeCall] public static partial void   UI_ModelListDirty(IntPtr Model, int ListField);

    // Cursor + input routing for this world's viewport.
    [NativeCall] public static partial void   UI_SetInputMode(ulong World, int Mode);
    [NativeCall] public static partial void   UI_SetMouseMode(ulong World, int Mode);
}
