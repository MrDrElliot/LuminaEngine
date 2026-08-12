using System;
using LuminaSharp;
using Lumina;

namespace Lumina.Examples;

/// <summary>
/// Composition + MVVM: one <see cref="ViewModel"/> drives three separate <c>&lt;template&gt;</c> widgets
/// (Clock, HealthBar, Minimap) that the HUD document slots in.
///
/// The widgets bind to the model of whatever document composes them, so they carry no script of their own.
/// This script only animates view-model properties; the composed widgets update themselves.
///
/// To run it: add a C# Script component to any entity, point it at <c>Lumina.Examples.HudExample</c>, and
/// press Play. Open Composition/Hud.rml in the editor to slot the same widgets in yourself.
/// </summary>
public sealed class HudExample : EntityScript
{
    [Property(Tooltip = "Composed HUD document.")]
    public string Document = "/Engine/Resources/Content/UI/Examples/Composition/HudComposed.rml";

    private sealed class HudModel : ViewModel
    {
        private string _Time = "00:00";
        private int _Health = 100;
        private int _BlipX = 84;
        private int _BlipY = 50;

        /// <summary>Clock readout ({{ Time }} in Clock.rml).</summary>
        [Bind] public string Time { get => _Time; set => Set(ref _Time, value); }

        /// <summary>0..100; drives the health bar fill width and its label.</summary>
        [Bind] public int Health { get => _Health; set => Set(ref _Health, value); }

        /// <summary>Minimap blip position, percent of the map (0..100).</summary>
        [Bind] public int BlipX { get => _BlipX; set => Set(ref _BlipX, value); }
        [Bind] public int BlipY { get => _BlipY; set => Set(ref _BlipY, value); }
    }

    private HudModel _Model = null!;
    private UIDataModel? _Binding;
    private UIDocument _Hud;

    public override void OnReady()
    {
        _Model = new HudModel();

        // Register the model BEFORE loading the document that binds to it.
        _Binding = World.UI.AddModel("hud", _Model);
        if (!_Binding.IsValid)
        {
            Debug.LogError("HudExample: failed to register the 'hud' data model.");
            return;
        }

        _Hud = World.UI.LoadDocument(Document);
        if (!_Hud.IsValid)
        {
            Debug.LogError($"HudExample: failed to load HUD document '{Document}'.");
            return;
        }

        _Hud.Show();

        // A HUD is not interactive, so the cursor stays captured for mouselook. Nothing to set here: that
        // is already the default input mode during play.
        Debug.Log("HudExample: HUD shown. Clock, health and minimap all update from one model.");
    }

    public override void OnUpdate(float DeltaTime)
    {
        // IsBound is false if registration failed, or after the model is disposed.
        if (!_Model.IsBound)
        {
            return;
        }

        float T = (float)LuminaSharp.Time.Now;

        // Running mm:ss clock from the world time.
        int Seconds = (int)T;
        _Model.Time = $"{(Seconds / 60) % 60:00}:{Seconds % 60:00}";

        // Health eases between 20 and 100 on a slow cycle. Set() only pushes when the int actually changes,
        // so this does not reflow the document every frame.
        _Model.Health = (int)Mathf.Lerp(20.0f, 100.0f, 0.5f * (1.0f + MathF.Sin(T * 0.6f)));

        // Blip orbits the minimap center (50,50) at radius 34%.
        _Model.BlipX = (int)(50.0f + 34.0f * MathF.Cos(T * 1.2f));
        _Model.BlipY = (int)(50.0f + 34.0f * MathF.Sin(T * 1.2f));
    }

    public override void OnDetach()
    {
        _Hud.Close();
        _Binding?.Dispose();
    }
}
