#include "Includes/SceneGlobals.slang"
#include "Includes/WorldDistanceField.slang"

// The world distance field ray marched from the camera, so what DF AO and far shadows trace can be seen directly.

// Mirror of Lumina::FWorldDistanceFieldGPU, the only argument.
struct FPushConstants
{
    FWorldDistanceField Field;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

static const uint  kMaxSteps = 256u;
// A step hits once the field is this share of a voxel from a surface, close enough that the level's own blur hides the gap.
static const float kHitVoxels = 0.25;

// The finest level red through to the coarsest violet, so a hit shows which level answered.
static const float3 kLevelTints[kWorldDFLevels] =
{
    float3(0.95, 0.30, 0.25),
    float3(0.95, 0.65, 0.20),
    float3(0.90, 0.90, 0.25),
    float3(0.35, 0.85, 0.30),
    float3(0.25, 0.80, 0.85),
    float3(0.30, 0.45, 0.95),
    float3(0.70, 0.40, 0.95),
};

// Read on the level that answered, so the gradient never mixes two resolutions.
float3 FieldNormal(FWorldDistanceField* Field, float3 P, uint Level)
{
    const float E = Field.VoxelSize(Level) * 0.5;
    const float3 Gradient = float3(Field.SampleLevel(Level, P + float3(E, 0.0, 0.0)) - Field.SampleLevel(Level, P - float3(E, 0.0, 0.0)),
                                   Field.SampleLevel(Level, P + float3(0.0, E, 0.0)) - Field.SampleLevel(Level, P - float3(0.0, E, 0.0)),
                                   Field.SampleLevel(Level, P + float3(0.0, 0.0, E)) - Field.SampleLevel(Level, P - float3(0.0, 0.0, E)));
    return dot(Gradient, Gradient) > 1e-12 ? normalize(Gradient) : float3(0.0, 1.0, 0.0);
}

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

[shader("fragment")]
float4 FragmentMain(FragmentInput In) : SV_Target0
{
    FWorldDistanceField* Field = &Pass().Field;
    // Dark red while the field is off or still waiting for its first full update.
    if (Field.NumLevels == 0u)
    {
        return float4(0.25, 0.02, 0.02, 1.0);
    }

    const float3 Origin = GetCameraPosition();
    const float3 Dir    = normalize(ReconstructWorldPos(In.UV, 0.5) - Origin);

    float t = 0.0;
    [loop]
    for (uint Step = 0u; Step < kMaxSteps; ++Step)
    {
        const float3 P = Origin + Dir * t;
        uint Level;
        const float Distance = Field.Sample(P, Level);
        if (Level >= kWorldDFLevels)
        {
            break;
        }
        const float Voxel = Field.VoxelSize(Level);
        if (Distance < kHitVoxels * Voxel)
        {
            const float3 N     = FieldNormal(Field, P, Level);
            const float  Shade = 0.25 + 0.75 * saturate(dot(N, normalize(float3(0.4, 0.8, 0.3))));
            return float4(kLevelTints[Level] * Shade, 1.0);
        }
        t += max(Distance, kHitVoxels * Voxel);
    }

    // Empty space, darker toward the ground so the horizon stays readable.
    return float4(lerp(float3(0.03, 0.03, 0.04), float3(0.10, 0.11, 0.14), saturate(Dir.y * 0.5 + 0.5)), 1.0);
}
