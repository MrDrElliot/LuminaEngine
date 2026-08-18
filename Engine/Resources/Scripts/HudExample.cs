using System;
using LuminaSharp;
using Lumina;

namespace Lumina.Examples;

// One model drives four composed <template> widgets: StatBar, Radar, Objective and Gauge.
public sealed class HudExample : EntityScript
{
    [Property(Tooltip = "Composed HUD. Open Composition/Hud.rml to slot the same widgets in yourself.")]
    public string Document = "/Engine/Resources/Content/UI/Examples/Composition/HudComposed.rml";

    private sealed class HudModel : ViewModel
    {
        private int _Health = 100;
        private int _Speed;
        private float _SpeedAngle = -120.0f;
        private float _SweepAngle;
        private int _BlipX = 70;
        private int _BlipY = 40;
        private string _Objective = "Reach the relay tower";
        private bool _Urgent;

        // StatBar reads this and flips its own .low class below 25, with no help from here.
        [Bind] public int Health { get => _Health; set => Set(ref _Health, value); }

        [Bind] public int Speed { get => _Speed; set => Set(ref _Speed, value); }

        // Gauge feeds this straight into data-style-transform as a rotate().
        [Bind] public float SpeedAngle { get => _SpeedAngle; set => Set(ref _SpeedAngle, value); }

        [Bind] public float SweepAngle { get => _SweepAngle; set => Set(ref _SweepAngle, value); }
        [Bind] public int BlipX { get => _BlipX; set => Set(ref _BlipX, value); }
        [Bind] public int BlipY { get => _BlipY; set => Set(ref _BlipY, value); }
        [Bind] public string Objective { get => _Objective; set => Set(ref _Objective, value); }
        [Bind] public bool Urgent { get => _Urgent; set => Set(ref _Urgent, value); }
    }

    private HudModel _Model = null!;
    private UIDataModel? _Binding;
    private UIDocument _Hud;

    public override void OnReady()
    {
        _Model = new HudModel();

        // Must precede LoadDocument: RmlUi resolves data bindings while parsing, and never retries.
        _Binding = World.UI.AddModel("hud", _Model);
        if (!_Binding.IsValid)
        {
            Debug.LogError("HudExample: failed to register the 'hud' data model.");
            return;
        }

        _Hud = World.UI.LoadDocument(Document);
        if (!_Hud.IsValid)
        {
            Debug.LogError($"HudExample: failed to load '{Document}'.");
            return;
        }

        _Hud.Show();

        // No EnableCursor here: a HUD is not interactive, so the mouse stays captured for look.
        Debug.Log("HudExample: HUD shown. Four widgets, one model, no script on any of them.");
    }

    public override void OnUpdate(float DeltaTime)
    {
        if (!_Model.IsBound)
        {
            return;
        }

        float T = (float)LuminaSharp.Time.Now;

        _Model.Health = (int)Mathf.Lerp(12.0f, 100.0f, 0.5f * (1.0f + MathF.Sin(T * 0.35f)));
        _Model.Urgent = _Model.Health < 25;

        int Kph = (int)Mathf.Lerp(0.0f, 240.0f, 0.5f * (1.0f + MathF.Sin(T * 0.8f)));
        _Model.Speed = Kph;

        // The dial sweeps 240 degrees from the 7 o'clock position, rounded so it pushes at most once a degree.
        _Model.SpeedAngle = MathF.Round(-120.0f + 240.0f * (Kph / 240.0f));

        // Any change relayouts the whole document, so a continuous angle is quantised the same way.
        _Model.SweepAngle = MathF.Round((T * 90.0f) % 360.0f);
        _Model.BlipX = (int)(50.0f + 32.0f * MathF.Cos(T * 1.1f));
        _Model.BlipY = (int)(50.0f + 32.0f * MathF.Sin(T * 1.1f));

        _Model.Objective = _Model.Urgent ? "Fall back to the relay tower" : "Reach the relay tower";
    }

    public override void OnDetach()
    {
        _Hud.Close();
        _Binding?.Dispose();
    }
}
