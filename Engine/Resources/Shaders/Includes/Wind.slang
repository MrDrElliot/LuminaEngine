#pragma once
#include "SceneGlobals.slang"

// Vertex wind animation. Displaces world-space vertices along a horizontal wind direction using a
// multi-octave procedural wind field, so foliage, cloth and grass move without any skeletal rig,
// vertex animation texture or CPU work.
//
// Consumed by the material graph's WindAnimation node, which runs in the VERTEX stage of every
// template carrying the material vertex token (MeshletVertex / MeshletMesh / the two VisBuffer
// geometry variants / DeferredMaterial's reconstruction loop / TerrainBaseVertexPass) and reads the
// vertex alias contract (WorldPosition, and GetTime / GetCameraPosition from SceneGlobals).
//
// Do not write that token's literal text anywhere in this file or the templates: substitution is a
// plain first-match find over the raw template source, so a mention in a comment gets replaced with
// the graph body instead of the real site.
//
// The field is built from sines rather than texture noise on purpose. This runs per VERTEX on
// potentially dense foliage, in six templates including the depth and shadow passes, so a texture
// fetch here costs a dependent read on geometry the rasterizer has not even reached yet. The sine
// sum is pure ALU, has no filtering or mip concerns, and is C-infinity continuous, which matters:
// any discontinuity in the field becomes a visible crack between adjacent vertices.
//
// EVERY pass that reconstructs geometry must apply the identical displacement, which is why the LOD
// gate below keys off the main camera position (GetCameraPosition) and not the rendering view. A
// shadow pass that gated on the light's position would fade its geometry out at a different distance
// than the base pass faded the surface, and the mesh would visibly detach from its own shadow.

struct FWindResult
{
    float3 Offset;   // world-space displacement; add to the vertex position (WorldPositionOffset)
    float  Weight;   // combined LOD + mask weight actually applied, 0 = wind fully culled
    float  Noise;    // raw multi-frequency wind field in [-1, 1], before mask/LOD scaling
};

// One octave of the wind field: two sines along incommensurate directions, advancing in time. The
// directions are deliberately not axis-aligned and not rational multiples of each other, so the two
// waves never re-phase into a visible grid the way dot(P, float3(1,0,0)) + dot(P, float3(0,0,1))
// would. Returns [-1, 1].
float WindWave(float3 P, float T)
{
    float A = sin(dot(P, float3(0.71, 0.33, 0.62)) + T);
    float B = sin(dot(P, float3(-0.41, 0.55, 0.73)) + T * 1.37 + 2.1);
    return (A + B) * 0.5;
}

// Multi-frequency (fBm) wind field. Octaves stack progressively finer, weaker waves on the base
// frequency: the low octave is the whole-plant sway, the middle ones branch motion, the high ones
// leaf flutter. One frequency alone reads as a mechanical pendulum, which is the single thing that
// makes procedural wind look fake.
//
// Each octave also advances FASTER in time (the 0.63 term), not just finer in space. Real turbulence
// breaks a gust into smaller eddies that move quicker; holding the time scale fixed across octaves
// makes the detail read as a static pattern painted onto the low-frequency sway.
//
// Normalized by the amplitude sum, so the result stays in [-1, 1] for any Octaves/Gain combination
// and changing the octave count re-times the motion without changing how far anything moves.
float WindTurbulence(float3 P, float T, float Frequency, float Lacunarity, float Gain, int Octaves, float Phase)
{
    float Sum  = 0.0;
    float Norm = 0.0;
    float Amp  = 1.0;
    float Freq = Frequency;

    for (int i = 0; i < Octaves; ++i)
    {
        Sum  += Amp * WindWave(P * Freq, T * (1.0 + float(i) * 0.63) + Phase);
        Norm += Amp;
        Freq *= Lacunarity;
        Amp  *= Gain;
    }

    return Sum / max(Norm, 1e-4);
}

// Distance LOD gate, from the MAIN camera (see the header note on pass consistency). 1 inside
// FadeStart, ramping to 0 at FadeEnd. Beyond that the caller skips the octave loop entirely, so this
// is a real vertex-rate saving on distant instances and not just a visual fade.
//
// Fading to zero rather than snapping matters for more than aesthetics: a hard cutoff would move
// vertices between frames as the camera crosses the threshold, and on a mesh straddling the boundary
// it would tear adjacent triangles apart.
float WindLODWeight(float3 WorldPos, float FadeStart, float FadeEnd)
{
    float D    = distance(GetCameraPosition(), WorldPos);
    float Span = max(FadeEnd - FadeStart, 1e-3);
    return 1.0 - saturate((D - FadeStart) / Span);
}

// Full wind displacement.
//
// Mask is the per-vertex stiffness weight (typically vertex color or a UV gradient): 0 pins a vertex
// in place, 1 lets it move the full Strength. Without it the whole mesh translates rigidly, roots and
// all. Phase desynchronizes instances that would otherwise sway in lockstep -- drive it from object
// position or entity ID.
//
// Gustiness in [0, 1] mixes in a low-frequency envelope travelling ALONG the wind. This is a separate
// band from the fBm octaves rather than one more of them because a gust MODULATES the whole
// displacement (a lull genuinely stills the plant) instead of adding another additive wiggle to it.
FWindResult ComputeWind(
    float3 WorldPos,
    float3 WindDir,
    float  Strength,
    float  Speed,
    float  Frequency,
    float  Lacunarity,
    float  Gain,
    int    Octaves,
    float  Mask,
    float  Phase,
    float  Gustiness,
    float  LODWeight)
{
    FWindResult Result;
    Result.Offset = float3(0.0, 0.0, 0.0);
    Result.Weight = 0.0;
    Result.Noise  = 0.0;

    float W = saturate(LODWeight) * max(Mask, 0.0);
    if (W <= 0.0)
    {
        // Culled by the LOD gate or masked out. Everything below is skipped, including the octave
        // loop -- this is the branch the gate exists to take.
        return Result;
    }

    float T = GetTime() * Speed;

    // Flatten the wind into the horizontal plane. A vertical component would drive vertices straight
    // through the surface the mesh is planted on rather than bending it, and no amount of authored
    // direction makes that read as wind. Degenerate (purely vertical) input falls back to +X.
    float3 Dir     = float3(WindDir.x, 0.0, WindDir.z);
    float  DirLen2 = dot(Dir, Dir);
    Dir = DirLen2 > 1e-6 ? Dir * rsqrt(DirLen2) : float3(1.0, 0.0, 0.0);

    // Gust envelope: a plane wave travelling along Dir at a quarter of the base frequency. Keying its
    // phase on dot(WorldPos, Dir) is what makes a field ripple in bands rolling across it, instead of
    // every instance swelling and lulling together.
    float Gust    = 0.5 + 0.5 * sin(dot(WorldPos, Dir) * Frequency * 0.25 - T * 0.6 + Phase);
    float GustAmp = lerp(1.0, Gust, saturate(Gustiness));

    Result.Noise = WindTurbulence(WorldPos, T, Frequency, Lacunarity, Gain, Octaves, Phase);

    float Bend = Result.Noise * GustAmp;

    // Bending is a ROTATION about the vertex's anchor, not a translation, so a lateral push has to
    // come with the matching drop or the mesh stretches as it leans -- most visible on grass, whose
    // blades grow measurably taller at full sway if this is left out. Taking the displaced point onto
    // the unit arc (x^2 + y^2 = 1) recovers the drop for the cost of one sqrt.
    float Lateral = Bend;
    float Drop    = 1.0 - sqrt(saturate(1.0 - Lateral * Lateral));

    Result.Offset = (Dir * Lateral - float3(0.0, Drop, 0.0)) * Strength * W;
    Result.Weight = W;
    return Result;
}
