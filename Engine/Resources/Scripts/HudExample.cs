using System;
using LuminaSharp;
using Lumina;

namespace Lumina.Examples;

// One model drives four composed <template> widgets, and as a UIScript the script itself is that model, so nothing here sets the UI up.
[UIDocument("/Engine/Resources/Content/UI/Examples/Composition/HudComposed.rml"), DataModel("hud")]
public sealed class HudExample : UIScript
{
    // StatBar reads this and flips its own .low class below 25, with no help from here.
    [Bind] public int Health = 100;

    [Bind] public int Speed;

    // Gauge feeds this straight into data-style-transform as a rotate().
    [Bind] public float SpeedAngle = -120.0f;

    [Bind] public float SweepAngle;
    [Bind] public int BlipX = 70;
    [Bind] public int BlipY = 40;
    [Bind] public string Objective = "Reach the relay tower";
    [Bind] public bool Urgent;

    public override void OnUpdate(float DeltaTime)
    {
        float T = (float)LuminaSharp.Time.Now;

        // Plain assignments. The UI compares the bound fields once a frame and pushes only what changed.
        Health = (int)Mathf.Lerp(12.0f, 100.0f, 0.5f * (1.0f + MathF.Sin(T * 0.35f)));
        Urgent = Health < 25;

        int Kph = (int)Mathf.Lerp(0.0f, 240.0f, 0.5f * (1.0f + MathF.Sin(T * 0.8f)));
        Speed = Kph;

        // The dial sweeps 240 degrees from the 7 o'clock position, rounded so it changes at most once a degree.
        SpeedAngle = MathF.Round(-120.0f + 240.0f * (Kph / 240.0f));

        // Any change relayouts the whole document, so a continuous angle is quantized the same way.
        SweepAngle = MathF.Round((T * 90.0f) % 360.0f);
        BlipX = (int)(50.0f + 32.0f * MathF.Cos(T * 1.1f));
        BlipY = (int)(50.0f + 32.0f * MathF.Sin(T * 1.1f));

        Objective = Urgent ? "Fall back to the relay tower" : "Reach the relay tower";
    }
}
