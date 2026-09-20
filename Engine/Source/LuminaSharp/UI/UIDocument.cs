using System;

namespace LuminaSharp;

/// <summary>
/// A loaded RmlUi document in a world's screen context (returned by <see cref="UI.LoadDocument"/>). A
/// lightweight handle: copy it freely; it stays valid until <see cref="Close"/> (or the world tears down).
/// </summary>
public readonly struct UIDocument
{
    internal readonly ulong World;
    internal readonly Lumina.FUIDocument Handle;

    internal UIDocument(ulong World, Lumina.FUIDocument Handle)
    {
        this.World = World;
        this.Handle = Handle;
    }

    /// <summary>False if the load failed or the document has been closed.</summary>
    public bool IsValid => Handle.IsValid;

    /// <summary>Make the document visible. <paramref name="Modal"/> blocks focus to other documents;
    /// <paramref name="AutoFocus"/> focuses the document's first autofocus element.</summary>
    public void Show(bool Modal = false, bool AutoFocus = true)
    {
        if (IsValid)
        {
            Lumina.CUILibrary.ShowDocument(Handle, Modal, AutoFocus);
        }
    }

    /// <summary>Hide the document without unloading it (keeps element handles + listeners valid).</summary>
    public void Hide()
    {
        if (IsValid)
        {
            Lumina.CUILibrary.HideDocument(Handle);
        }
    }

    /// <summary>Unload and destroy the document. Element handles from it become invalid; dispose any
    /// <see cref="UIEventSubscription"/> on its elements first.</summary>
    public void Close()
    {
        if (IsValid)
        {
            Lumina.CUILibrary.UnloadDocument(UI.WorldOf(World), Handle);
        }
    }

    /// <summary>Raise this document above the others in the context (z-order).</summary>
    public void BringToFront()
    {
        if (IsValid)
        {
            Lumina.CUILibrary.BringDocumentToFront(Handle);
        }
    }

    /// <summary>The document's root element (the body), e.g. to attach a document-wide event listener.</summary>
    public UIElement Root => new(World, IsValid ? Lumina.CUILibrary.GetDocumentRoot(Handle) : default);

    /// <summary>The element with the given <c>id</c>, or an invalid element if absent.</summary>
    public UIElement GetElementById(string Id) => new(World, IsValid ? Lumina.CUILibrary.GetElementById(Handle, Id) : default);

    /// <summary>Shorthand for <see cref="GetElementById"/>: <c>document["score"]</c>.</summary>
    public UIElement this[string Id] => GetElementById(Id);

    /// <summary>First element matching a CSS selector (e.g. ".health-bar > .fill"), or an invalid element.</summary>
    public UIElement Query(string Selector) => new(World, IsValid ? Lumina.CUILibrary.QuerySelector(Root.Handle, Selector) : default);
}
