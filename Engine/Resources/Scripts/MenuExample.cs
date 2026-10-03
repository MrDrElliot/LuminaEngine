using System;
using LuminaSharp;
using Lumina;

namespace Lumina.Examples;

// Drives UI/Examples/Menu.rml plus the Settings dialog it opens, both bound to one model.
public sealed class MenuExample : EntityScript
{
    [Property(Tooltip = "Main menu document.")]
    public string MenuDocument = "/Engine/Resources/Content/UI/Examples/Menu.rml";

    [Property(Tooltip = "Settings dialog, composed inside Window.rml's chrome.")]
    public string SettingsDocument = "/Engine/Resources/Content/UI/Examples/Composition/Settings.rml";

    private sealed class MenuModel : ViewModel
    {
        private string _Tagline = "Press Continue, or wander the settings.";
        private string _Profile = "commander.lumina";
        private bool _Online = true;
        private bool _Dark = true;
        private bool _Confirming;
        private int _PlayCount;

        private string _Title = "Settings";
        private int _Volume = 70;
        private bool _Fullscreen = CGameUserSettingsLibrary.GetWindowMode() != EWindowMode.Windowed;
        private bool _VSync = CGameUserSettingsLibrary.GetVSync();
        private string _Quality = QualityName(CGameUserSettingsLibrary.GetOverallQuality());

        [Bind] public string Tagline { get => _Tagline; set => Set(ref _Tagline, value); }
        [Bind] public string Profile { get => _Profile; set => Set(ref _Profile, value); }
        [Bind] public bool Online { get => _Online; set => Set(ref _Online, value); }

        // Read by the modal's data-if, which drops the overlay out of the tree entirely when false.
        [Bind] public bool Confirming { get => _Confirming; set => Set(ref _Confirming, value); }

        // The button label is the NEXT action, so it is computed and must be pushed by hand.
        [Bind] public string ThemeLabel => _Dark ? "Light theme" : "Dark theme";

        // Window.rml reads this through the composing document's model, so its chrome needs no script.
        [Bind] public string Title => _Title;

        [Bind] public int Volume { get => _Volume; set { Set(ref _Volume, value); Debug.Log($"[Menu] Volume -> {value}"); } }
        // Each choice applies at once so the player sees it, and Done is what keeps it.
        [Bind]
        public bool Fullscreen
        {
            get => _Fullscreen;
            set
            {
                Set(ref _Fullscreen, value);
                CGameUserSettingsLibrary.SetWindowMode(value ? EWindowMode.BorderlessFullscreen : EWindowMode.Windowed);
                CGameUserSettingsLibrary.ApplySettings();
            }
        }

        [Bind]
        public bool VSync
        {
            get => _VSync;
            set
            {
                Set(ref _VSync, value);
                CGameUserSettingsLibrary.SetVSync(value);
                CGameUserSettingsLibrary.ApplySettings();
            }
        }

        [Bind]
        public string Quality
        {
            get => _Quality;
            set
            {
                Set(ref _Quality, value);
                if (Enum.TryParse(value, true, out EQualityLevel Level) && Level != EQualityLevel.Custom)
                {
                    CGameUserSettingsLibrary.SetOverallQuality(Level);
                    CGameUserSettingsLibrary.ApplySettings();
                }
            }
        }

        private static string QualityName(EQualityLevel Level) => Level.ToString().ToLowerInvariant();

        public Action? OnOpenSettings;
        public Action? OnCloseSettings;
        public Action? OnQuit;

        [BindCommand]
        public void Play()
        {
            _PlayCount++;
            Tagline = _PlayCount >= 3 ? "Loading..." : $"Play pressed {_PlayCount}x";
        }

        [BindCommand]
        public void ToggleTheme()
        {
            _Dark = !_Dark;
            NotifyChanged(nameof(ThemeLabel));
            Tagline = _Dark ? "Dark theme" : "Light theme";
        }

        [BindCommand] public void OpenSettings() => OnOpenSettings?.Invoke();
        [BindCommand]
        public void CloseSettings()
        {
            CGameUserSettingsLibrary.SaveSettings();
            OnCloseSettings?.Invoke();
        }

        [BindCommand]
        public void RevertSettings()
        {
            CGameUserSettingsLibrary.RevertSettings();
            Set(ref _Fullscreen, CGameUserSettingsLibrary.GetWindowMode() != EWindowMode.Windowed, nameof(Fullscreen));
            Set(ref _VSync, CGameUserSettingsLibrary.GetVSync(), nameof(VSync));
            Set(ref _Quality, QualityName(CGameUserSettingsLibrary.GetOverallQuality()), nameof(Quality));
        }
        [BindCommand] public void Confirm() => Confirming = true;
        [BindCommand] public void Cancel() => Confirming = false;
        [BindCommand] public void Quit() { Confirming = false; OnQuit?.Invoke(); }

        [BindCommand]
        public void ApplyPreset(string Preset)
        {
            Quality = Preset;
            Volume = Preset == "low" ? 30 : 90;
        }
    }

    private MenuModel _Model = null!;
    private UIDataModel? _Binding;
    private UIDocument _Menu;
    private UIDocument _Settings;

    public override void OnReady()
    {
        _Model = new MenuModel();

        // Must precede LoadDocument: RmlUi resolves data bindings while parsing, and never retries.
        _Binding = World.UI.AddModel("menu", _Model);
        if (!_Binding.IsValid)
        {
            Debug.LogError("MenuExample: failed to register the 'menu' data model.");
            return;
        }

        _Menu = World.UI.LoadDocument(MenuDocument);
        if (!_Menu.IsValid)
        {
            Debug.LogError($"MenuExample: failed to load '{MenuDocument}'.");
            return;
        }
        _Menu.Show();

        // Both documents live in the same context, so both resolve the same "menu" model.
        _Settings = World.UI.LoadDocument(SettingsDocument);
        if (!_Settings.IsValid)
        {
            Debug.LogError($"MenuExample: failed to load '{SettingsDocument}'.");
        }

        _Model.OnOpenSettings = () => { _Settings.Show(); _Settings.BringToFront(); };
        _Model.OnCloseSettings = () => _Settings.Hide();
        _Model.OnQuit = () => Debug.Log("MenuExample: Quit confirmed.");

        World.UI.EnableCursor();
        Debug.Log("MenuExample: menu shown. Arrow keys navigate; Quit opens a data-if modal.");
    }

    public override void OnDetach()
    {
        _Settings.Close();
        _Menu.Close();
        _Binding?.Dispose();
        World.UI.DisableCursor();
    }
}
