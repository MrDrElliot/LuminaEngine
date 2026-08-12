using System;
using LuminaSharp;
using Lumina;

namespace Lumina.Examples;

/// <summary>
/// TWO-WAY binding and commands with arguments (Composition/Settings.rml).
///
/// The form controls write straight back into the <see cref="ViewModel"/>: typing in a field
/// (<c>data-value</c>) or toggling a checkbox (<c>data-checked</c>) runs the C# property setter, which logs
/// the new value, and the <c>{{ }}</c> readouts refresh live. The preset buttons call one command with an
/// argument.
///
/// To run it: add a C# Script component to any entity, point it at <c>Lumina.Examples.SettingsExample</c>,
/// press Play, then edit the settings and watch the log.
/// </summary>
public sealed class SettingsExample : EntityScript
{
    [Property(Tooltip = "Settings document.")]
    public string Document = "/Engine/Resources/Content/UI/Examples/Composition/Settings.rml";

    private sealed class SettingsModel : ViewModel
    {
        private string _Title = "Settings";
        private int _MasterVolume = 80;
        private int _FieldOfView = 90;
        private bool _Fullscreen = true;
        private bool _VSync;

        /// <summary>Window chrome title ({{ Title }} in Window.rml). Get-only, so it is display-only.</summary>
        [Bind] public string Title => _Title;

        // Two-way: these setters run when the user edits the control. The logging proves the writeback
        // reaches C#. A [Bind] property is two-way purely because it HAS a setter.
        [Bind] public int MasterVolume { get => _MasterVolume; set { Set(ref _MasterVolume, value); Debug.Log($"[Settings] MasterVolume -> {value}"); } }
        [Bind] public int FieldOfView { get => _FieldOfView; set { Set(ref _FieldOfView, value); Debug.Log($"[Settings] FieldOfView -> {value}"); } }
        [Bind] public bool Fullscreen { get => _Fullscreen; set { Set(ref _Fullscreen, value); Debug.Log($"[Settings] Fullscreen -> {value}"); } }
        [Bind] public bool VSync { get => _VSync; set { Set(ref _VSync, value); Debug.Log($"[Settings] VSync -> {value}"); } }

        /// <summary>A command WITH an argument: the RML calls ApplyPreset('low') / ApplyPreset('high').
        /// Arguments cross as strings and are converted to the parameter type.</summary>
        [BindCommand]
        public void ApplyPreset(string Preset)
        {
            Debug.Log($"[Settings] preset -> {Preset}");
            if (Preset == "low")
            {
                MasterVolume = 40;
                FieldOfView = 80;
                VSync = false;
            }
            else
            {
                MasterVolume = 100;
                FieldOfView = 110;
                VSync = true;
            }
        }
    }

    private SettingsModel _Model = null!;
    private UIDataModel? _Binding;
    private UIDocument _Settings;

    public override void OnReady()
    {
        _Model = new SettingsModel();

        _Binding = World.UI.AddModel("settings", _Model);
        if (!_Binding.IsValid)
        {
            Debug.LogError("SettingsExample: failed to register the 'settings' data model.");
            return;
        }

        _Settings = World.UI.LoadDocument(Document);
        if (!_Settings.IsValid)
        {
            Debug.LogError($"SettingsExample: failed to load settings document '{Document}'.");
            return;
        }

        _Settings.Show();

        // A settings screen needs a clickable cursor and keyboard focus for the text fields.
        World.UI.EnableCursor();
        Debug.Log("SettingsExample: settings shown. Edit a field or toggle a box; watch the log.");
    }

    public override void OnDetach()
    {
        _Settings.Close();
        _Binding?.Dispose();
        World.UI.DisableCursor();
    }
}
