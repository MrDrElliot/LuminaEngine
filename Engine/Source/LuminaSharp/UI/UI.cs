using System;

namespace LuminaSharp;

/// <summary>
/// A world's UI interface (<c>World.UI</c>): load and present screen-space RmlUi documents and route the
/// cursor between gameplay and the UI. The C# mirror of the other gameplay facades; backed by the
/// <c>LuminaSharp_UI_*</c> exports. Game thread only. Documents render full-screen over this world's view
/// (the same context mouse/keyboard input is already forwarded to), distinct from world-space
/// <c>SWidgetComponent</c> billboards.
/// </summary>
public readonly unsafe partial struct UI
{
    internal readonly ulong Handle; // CWorld*

    internal UI(ulong Handle)
    {
        this.Handle = Handle;
    }

    public bool IsValid => Handle != 0;

    private Lumina.CWorld World => WorldOf(Handle);

    internal static Lumina.CWorld WorldOf(ulong Handle) => Wrapper<Lumina.CWorld>.ForObject((IntPtr)Handle)!;

    /// <summary>
    /// Loads the RML document at <paramref name="Path"/> (a virtual path, e.g.
    /// "/Game/UI/HUD.rml") into this world's screen context. The document starts hidden -- call
    /// <see cref="UIDocument.Show"/>. Returns an invalid <see cref="UIDocument"/> on load/parse failure.
    /// </summary>
    public UIDocument LoadDocument(string Path) => new(Handle, Lumina.CUILibrary.LoadDocument(World, Path));

    /// <summary>Loads a document from an in-memory RML string. <paramref name="SourceUrl"/> resolves relative
    /// includes (stylesheets, images). Useful for procedurally built UI.</summary>
    public UIDocument LoadDocumentFromMemory(string Rml, string SourceUrl = "[inline]")
        => new(Handle, Lumina.CUILibrary.LoadDocumentFromMemory(World, Rml, SourceUrl));

    /// <summary>
    /// Registers an MVVM <see cref="ViewModel"/> as a named data model on this world's UI context, so RML can
    /// bind to it declaratively (<c>data-model="Name"</c> + <c>{{ Prop }}</c> / <c>data-event-*</c>). Call this
    /// BEFORE <see cref="LoadDocument"/> for the document that references the model, RmlUi resolves data bindings
    /// at load time. Keep the returned <see cref="UIDataModel"/> and <see cref="UIDataModel.Dispose">dispose</see>
    /// it when done (e.g. in <see cref="EntityScript.OnDetach"/>). Returns an invalid model on failure (e.g. a
    /// duplicate name); check <see cref="UIDataModel.IsValid"/>.
    /// </summary>
    public UIDataModel AddModel(string Name, ViewModel Model) => new(Handle, Name, Model);

    /// <summary>
    /// Re-fetch a data model registered on this world by <see cref="AddModel"/>, so another script can push to
    /// a model it didn't create. Returns null if no model with that name is currently registered.
    /// </summary>
    public UIDataModel? GetModel(string Name) => UIDataModel.Find(Handle, Name);

    /// Shows a free cursor and lets the UI receive clicks while gameplay still gets the rest. Call when a menu opens.
    public void EnableCursor()
    {
        Lumina.CInputLibrary.SetInputMode(World, Lumina.EInputMode.GameAndUI);
        Lumina.CInputLibrary.SetMouseMode(World, Lumina.EMouseMode.Normal);
    }

    /// Hides and captures the cursor for mouselook and routes input back to gameplay. Call when a menu closes.
    public void DisableCursor()
    {
        Lumina.CInputLibrary.SetInputMode(World, Lumina.EInputMode.Game);
        Lumina.CInputLibrary.SetMouseMode(World, Lumina.EMouseMode.Captured);
    }
}
