using System;
using LuminaSharp;
using Lumina;

namespace Lumina.Examples;

/// <summary>
/// The smallest complete World.UI example: a screen-space menu driven by MVVM data binding.
///
/// The RML declares WHAT to show (<c>{{ Status }}</c>, <c>data-event-click="Play()"</c>,
/// <c>data-class-light="!Dark"</c>); this script only changes view-model properties and the view follows.
/// There are no element lookups and no listener wiring.
///
/// To run it: add a C# Script component to any entity in a game world, point it at
/// <c>Lumina.Examples.MenuExample</c>, and press Play. See Menu.rml / Menu.rcss beside this file under
/// Engine/Resources/Content/UI/Examples/.
/// </summary>
public sealed class MenuExample : EntityScript
{
    [Property(Tooltip = "RML document shown on screen.")]
    public string Document = "/Engine/Resources/Content/UI/Examples/Menu.rml";

    /// <summary>The view-model behind Menu.rml. Bound properties flow to the view; commands flow back from it.</summary>
    private sealed class MenuModel : ViewModel
    {
        private string _Status = "Click Play to begin.";
        private bool _Dark = true;
        private int _PlayCount;

        /// <summary>Status line text, shown via <c>{{ Status }}</c>.</summary>
        [Bind] public string Status { get => _Status; set => Set(ref _Status, value); }

        /// <summary>Dark theme on/off; drives <c>data-class-light="!Dark"</c> on the panel.</summary>
        [Bind] public bool Dark { get => _Dark; set => Set(ref _Dark, value); }

        /// <summary>The toggle button's label = the NEXT action. Computed, so it has no setter and is
        /// display-only; it must be re-pushed by hand when <see cref="Dark"/> flips.</summary>
        [Bind] public string ThemeLabel => _Dark ? "Light theme" : "Dark theme";

        // Commands invoked from RML via data-event-click. No element lookups, no listener wiring.
        [BindCommand]
        public void Play()
        {
            _PlayCount++;
            Status = _PlayCount >= 3 ? "Starting..." : $"Play clicked {_PlayCount}x";
        }

        [BindCommand]
        public void ToggleTheme()
        {
            Dark = !_Dark;                       // pushes Dark -> the panel restyles via data-class-light
            NotifyChanged(nameof(ThemeLabel));   // computed label depends on Dark, so push it too
            Status = _Dark ? "Dark theme" : "Light theme";
        }
    }

    private MenuModel _Model = null!;
    private UIDataModel? _Binding;
    private UIDocument _Menu;

    public override void OnReady()
    {
        _Model = new MenuModel();

        // Register the data model BEFORE loading the document: RmlUi resolves data bindings at load time,
        // so a document loaded first binds to nothing and never recovers.
        _Binding = World.UI.AddModel("menu", _Model);
        if (!_Binding.IsValid)
        {
            Debug.LogError("MenuExample: failed to register the 'menu' data model (is the name already taken?).");
            return;
        }

        _Menu = World.UI.LoadDocument(Document);
        if (!_Menu.IsValid)
        {
            Debug.LogError($"MenuExample: failed to load UI document '{Document}'.");
            return;
        }

        // Documents load hidden.
        _Menu.Show();

        // Free the cursor so the buttons are clickable; gameplay still receives the rest of the input.
        World.UI.EnableCursor();

        Debug.Log("MenuExample: menu shown. Click 'Play' or the theme button.");
    }

    public override void OnDetach()
    {
        // Tear down in reverse: close the document, then drop the model (frees the managed callback).
        _Menu.Close();
        _Binding?.Dispose();
        World.UI.DisableCursor();
    }
}
