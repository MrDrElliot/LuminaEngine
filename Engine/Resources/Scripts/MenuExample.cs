using System;
using LuminaSharp;
using Lumina;

namespace Lumina.Examples;

// Drives UI/Examples/Menu.rml plus the Settings dialog it opens, both bound to this script as the "menu" model.
[UIDocument("/Engine/Resources/Content/UI/Examples/Menu.rml", Interactive = true), DataModel("menu")]
public sealed class MenuExample : UIScript
{
    [Property(Tooltip = "Settings dialog, composed inside Window.rml's chrome.", AssetType = "rml")]
    public string SettingsDocument = "/Engine/Resources/Content/UI/Examples/Composition/Settings.rml";

    [Bind] public string Tagline = "Press Continue, or wander the settings.";
    [Bind] public string Profile = "commander.lumina";
    [Bind] public bool Online = true;

    // Read by the modal's data-if, which drops the overlay out of the tree entirely when false.
    [Bind] public bool Confirming;

    // Computed, so the engine reads it through its getter and compares it every frame.
    [Bind] public string ThemeLabel => _Dark ? "Light theme" : "Dark theme";

    // Window.rml reads this through the composing document's model, so its chrome needs no script.
    [Bind] public string Title = "Settings";

    [Bind] public int Volume = 70;

    // Each choice applies at once through its [Change] handler, which runs when the UI writes the field, and Done is what keeps it.
    [Bind, Change(nameof(ApplyFullscreen))] public bool Fullscreen;
    [Bind, Change(nameof(ApplyVSync))] public bool VSync;
    [Bind, Change(nameof(ApplyQuality))] public string Quality = "";

    private bool _Dark = true;
    private int _PlayCount;
    private UIDocument _Settings;

    private static string QualityName(EQualityLevel Level) => Level.ToString().ToLowerInvariant();

    public override void OnReady()
    {
        ReadSettings();
        Debug.Log("MenuExample: menu shown. Arrow keys navigate; Quit opens a data-if modal.");
    }

    // Loaded on first use, since a script hot reload drops this handle along with the rest of the managed state.
    private bool EnsureSettings()
    {
        if (!_Settings.IsValid)
        {
            // It binds to the "menu" model this script already registered, so it needs no model of its own.
            _Settings = World.UI.LoadDocument(SettingsDocument);
            if (!_Settings.IsValid)
            {
                Debug.LogError($"MenuExample: failed to load '{SettingsDocument}'.");
            }
        }
        return _Settings.IsValid;
    }

    public override void OnDetach()
    {
        _Settings.Close();
    }

    [Bind]
    public void Play()
    {
        _PlayCount++;
        Tagline = _PlayCount >= 3 ? "Loading..." : $"Play pressed {_PlayCount}x";
    }

    [Bind]
    public void ToggleTheme()
    {
        _Dark = !_Dark;
        Tagline = _Dark ? "Dark theme" : "Light theme";
    }

    [Bind]
    public void OpenSettings()
    {
        if (EnsureSettings())
        {
            _Settings.Show();
            _Settings.BringToFront();
        }
    }

    [Bind]
    public void CloseSettings()
    {
        CGameUserSettingsLibrary.SaveSettings();
        _Settings.Hide();
    }

    [Bind]
    public void RevertSettings()
    {
        CGameUserSettingsLibrary.RevertSettings();
        ReadSettings();
    }

    private void ReadSettings()
    {
        Fullscreen = CGameUserSettingsLibrary.GetWindowMode() != EWindowMode.Windowed;
        VSync = CGameUserSettingsLibrary.GetVSync();
        Quality = QualityName(CGameUserSettingsLibrary.GetOverallQuality());
    }

    private void ApplyFullscreen()
    {
        CGameUserSettingsLibrary.SetWindowMode(Fullscreen ? EWindowMode.BorderlessFullscreen : EWindowMode.Windowed);
        CGameUserSettingsLibrary.ApplySettings();
    }

    private void ApplyVSync()
    {
        CGameUserSettingsLibrary.SetVSync(VSync);
        CGameUserSettingsLibrary.ApplySettings();
    }

    private void ApplyQuality()
    {
        if (Enum.TryParse(Quality, true, out EQualityLevel Level) && Level != EQualityLevel.Custom)
        {
            CGameUserSettingsLibrary.SetOverallQuality(Level);
            CGameUserSettingsLibrary.ApplySettings();
        }
    }

    [Bind] public void Confirm() => Confirming = true;
    [Bind] public void Cancel() => Confirming = false;

    [Bind]
    public void Quit()
    {
        Confirming = false;
        Debug.Log("MenuExample: Quit confirmed.");
    }

    [Bind]
    public void ApplyPreset(string Preset)
    {
        Quality = Preset;
        ApplyQuality();
        Volume = Preset == "low" ? 30 : 90;
    }
}
