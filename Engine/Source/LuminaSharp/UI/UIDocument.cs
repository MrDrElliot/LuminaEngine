using System;

namespace LuminaSharp;

public readonly struct UIDocument : IDisposable
{
    internal readonly ulong World;
    internal readonly Lumina.SUIDocument Handle;

    internal UIDocument(ulong World, Lumina.SUIDocument Handle)
    {
        this.World = World;
        this.Handle = Handle;
    }

    public UIDocument(Lumina.CWorld World, Lumina.SUIDocument Handle)
        : this(World.WorldHandle, Handle)
    {
    }

    // False if the load failed or the document was closed.
    public bool IsValid => Handle.IsValid;

    // Modal blocks focus to other documents, and AutoFocus focuses the first autofocus element.
    public void Show(bool Modal = false, bool AutoFocus = true)
    {
        if (IsValid)
        {
            Lumina.CUILibrary.ShowDocument(Handle, Modal, AutoFocus);
        }
    }

    // Keeps element handles and listeners valid.
    public void Hide()
    {
        if (IsValid)
        {
            Lumina.CUILibrary.HideDocument(Handle);
        }
    }

    // Invalidates element handles, so dispose their UIEventSubscriptions first.
    public void Close()
    {
        if (IsValid)
        {
            UIWorld.FromHandle(World).UI.UnloadDocument(Handle);
        }
    }

    public void Dispose() => Close();

    public void BringToFront()
    {
        if (IsValid)
        {
            Lumina.CUILibrary.BringDocumentToFront(Handle);
        }
    }

    // The body, e.g. to attach a document-wide event listener.
    public ref readonly UIElement Root => ref UIElement.Hold(new(World, IsValid ? Lumina.CUILibrary.GetDocumentRoot(Handle) : default));

    // An invalid element if absent.
    public UIElement GetElementById(string Id) => new(World, IsValid ? Lumina.CUILibrary.GetElementById(Handle, Id) : default);

    public ref readonly UIElement this[string Id] => ref UIElement.Hold(GetElementById(Id));

    // First element matching a CSS selector, or an invalid element.
    public UIElement Query(string Selector) => new(World, IsValid ? Lumina.CUILibrary.QuerySelector(Root.Handle, Selector) : default);
}
