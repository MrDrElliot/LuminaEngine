using System;
using LuminaSharp;
using Lumina;

namespace LuminaExamples;

// Drives a dynamic material instance and a parameter collection from gameplay.
public class MaterialApiExample : EntityScript
{
    [Property(Category = "Flash", Tooltip = "Vector parameter on the mesh material to drive.")]
    public string ColorParameter = "BaseColor";

    [Property(Category = "Flash", Tooltip = "Color the surface flashes to when Hit is called.")]
    public FVector4 FlashColor = new FVector4(1.0f, 0.1f, 0.1f, 1.0f);

    [Property(Category = "Flash", Tooltip = "Seconds the flash takes to fade back to the original color.")]
    public float FlashDuration = 0.25f;

    [Property(Category = "Collection", Tooltip = "Collection this script drives; every material bound to it sees the write.")]
    public CMaterialParameterCollection? Collection;

    [Property(Category = "Collection", Tooltip = "Scalar the collection declares, driven from GlobalValue.")]
    public string CollectionParameter = "Wetness";

    [Property(Category = "Collection")]
    public float GlobalValue = 1.0f;

    private CMaterialInstance? Material;
    private FVector4 RestColor;
    private float FlashRemaining;

    public override void OnReady()
    {
        var Mesh = Registry.TryGet<SStaticMeshComponent>(Entity);
        if (Mesh == null)
        {
            Debug.Log("MaterialApiExample: no static mesh component on this entity.");
            return;
        }

        // Transient, so nothing written below reaches the material asset on disk.
        Material = Mesh.CreateDynamicMaterialInstance(0);
        if (Material == null)
        {
            return;
        }

        if (!Material.HasVectorParameter(ColorParameter))
        {
            Debug.Log($"MaterialApiExample: the material exposes no vector parameter '{ColorParameter}'.");
            Material = null;
            return;
        }

        RestColor = Material.GetVectorValue(ColorParameter, new FVector4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    // Flashes the surface, then fades it back over FlashDuration.
    public void Hit()
    {
        if (Material == null)
        {
            return;
        }

        FlashRemaining = FlashDuration;
        Material.SetVectorValue(ColorParameter, FlashColor);
    }

    public override void OnUpdate(float DeltaTime)
    {
        if (Material != null && FlashRemaining > 0.0f)
        {
            FlashRemaining -= DeltaTime;

            float Alpha = FlashDuration > 0.0f ? Math.Max(FlashRemaining, 0.0f) / FlashDuration : 0.0f;
            Material.SetVectorValue(ColorParameter, Lerp(RestColor, FlashColor, Alpha));
        }

        // One write, and every material binding this collection reads the new value on the next frame.
        Collection?.SetScalarValue(CollectionParameter, GlobalValue);
    }

    private static FVector4 Lerp(FVector4 A, FVector4 B, float T)
    {
        return new FVector4(A.X + (B.X - A.X) * T,
                            A.Y + (B.Y - A.Y) * T,
                            A.Z + (B.Z - A.Z) * T,
                            A.W + (B.W - A.W) * T);
    }
}
