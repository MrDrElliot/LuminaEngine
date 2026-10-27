#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// Mirrors FWeatherContactGPU in SceneRenderTypes.h.
struct FWeatherContact
{
    float3 Position;
    float  Radius;
    float3 PrevPosition;
    float  SurfaceY;
    float  PrevSurfaceY;
    float  RippleStrength;
    float  FoamStrength;
    float  TrailStrength;
    uint   Surface;
    uint   _Pad0;
    uint   _Pad1;
    uint   _Pad2;
};

// Mirrors FWeatherWaterBodyGPU in SceneRenderTypes.h.
struct FWeatherWaterBody
{
    // Rows of the world to body transform that give local x and z, inside the body while both stay within half a unit.
    float4 WorldToWaterX;
    float4 WorldToWaterZ;
    // x = surface height, y = interaction ripple scale, z = interaction foam scale, w = rain ripple scale.
    float4 Params;
};

struct FPushConstants
{
    GPUSpan<FWeatherContact>   Contacts;
    GPUSpan<FWeatherWaterBody> Waters;
    FTexture2D   Previous;
    FRWTexture2D Current;
    int2         Shift;
    float2       Origin;
    float        TexelWorld;
    uint         Resolution;
    float        Courant;
    float        WaterDamping;
    float        ShallowDamping;
    float        FoamDecay;
    float        TrailDecay;
    uint         bStep;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

static const uint  kSurfaceGround   = 1u;
static const uint  kSurfaceWater    = 2u;
static const uint  kGroupSize       = 8u;
static const uint  kMaxGroupContacts = 64u;
// Standing water on a puddle, matching kPuddleDepth in WeatherInteractionSystem.cpp.
static const float kPuddleDepth     = 0.015;
// How far a planted foot sinks into snow or mud, which sets how wide and soft its print is.
static const float kPrintSink       = 0.06;
// A 2D field holds no volume, so the water a contact moves is scaled down to the height it raises.
static const float kDisplacementGain = 0.35;
static const float kFoamGain         = 6.0;
static const float kMaxHeight        = 0.3;
// Bleeds the mean level back to rest, since the discrete wave equation slowly drifts.
static const float kRestPull         = 0.998;

groupshared uint gsContactCount;
groupshared uint gsContacts[kMaxGroupContacts];

float4 LoadState(int2 Texel)
{
    const int Size = int(Pass().Resolution);
    if (any(Texel < 0) || any(Texel >= Size))
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return Pass().Previous.Load(Texel);
}

// Vertical length of the sphere lying below the surface at a point, which is the water it holds aside there.
float SubmergedChord(float3 Center, float Radius, float SurfaceY, float2 Point)
{
    const float2 Delta = Point - Center.xz;
    const float  Half2 = Radius * Radius - dot(Delta, Delta);
    if (Half2 <= 0.0)
    {
        return 0.0;
    }
    const float Half = sqrt(Half2);
    return clamp(SurfaceY - (Center.y - Half), 0.0, 2.0 * Half);
}

bool ContactTouchesTile(FWeatherContact Contact, float2 TileMin, float2 TileMax)
{
    const float2 Now  = clamp(Contact.Position.xz, TileMin, TileMax) - Contact.Position.xz;
    const float2 Prev = clamp(Contact.PrevPosition.xz, TileMin, TileMax) - Contact.PrevPosition.xz;
    const float  R2   = Contact.Radius * Contact.Radius;
    return dot(Now, Now) <= R2 || dot(Prev, Prev) <= R2;
}

// Steps the damped wave equation over the window and adds the water each contact pushed aside since last frame.
[shader("compute")]
[numthreads(kGroupSize, kGroupSize, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID, uint3 GroupID : SV_GroupID, uint GroupIndex : SV_GroupIndex)
{
    if (GroupIndex == 0u)
    {
        gsContactCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const float2 TileMin = Pass().Origin + float2(GroupID.xy * kGroupSize) * Pass().TexelWorld;
    const float2 TileMax = TileMin + float(kGroupSize) * Pass().TexelWorld;
    const uint   ContactCount = Pass().Contacts.Count;
    for (uint Index = GroupIndex; Index < ContactCount; Index += kGroupSize * kGroupSize)
    {
        if (ContactTouchesTile(Pass().Contacts.Get(Index), TileMin, TileMax))
        {
            uint Slot;
            InterlockedAdd(gsContactCount, 1u, Slot);
            if (Slot < kMaxGroupContacts)
            {
                gsContacts[Slot] = Index;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const int2   Texel  = int2(DTid.xy);
    const int2   Source = Texel + Pass().Shift;
    const float2 World  = Pass().Origin + (float2(Texel) + 0.5) * Pass().TexelWorld;

    // Open water carries a wake for seconds, a puddle's thin film kills it within a stride.
    float WaterRipple = 0.0;
    float WaterFoam   = 0.0;
    for (uint Body = 0u; Body < Pass().Waters.Count; ++Body)
    {
        const FWeatherWaterBody Water = Pass().Waters.Get(Body);
        const float4 Point  = float4(World.x, Water.Params.x, World.y, 1.0);
        const float2 Local  = float2(dot(Water.WorldToWaterX, Point), dot(Water.WorldToWaterZ, Point));
        if (all(abs(Local) <= 0.5))
        {
            WaterRipple = max(WaterRipple, Water.Params.y);
            WaterFoam   = max(WaterFoam, Water.Params.z);
        }
    }
    const bool bOpenWater = WaterRipple > 0.0;

    float4 State = LoadState(Source);
    float  Height = State.r;
    float  Last   = State.g;
    float  Foam   = State.b;
    float  Trail  = State.a;

    if (Pass().bStep != 0u)
    {
        const float Laplacian = LoadState(Source + int2(-1, 0)).r + LoadState(Source + int2(1, 0)).r
                              + LoadState(Source + int2(0, -1)).r + LoadState(Source + int2(0, 1)).r - 4.0 * Height;
        const float Damping = bOpenWater ? Pass().WaterDamping : Pass().ShallowDamping;
        const float Next    = (Height + (Height - Last) * Damping + Pass().Courant * Laplacian) * kRestPull;
        Last   = Height;
        Height = Next;
        Foam  *= Pass().FoamDecay;
        Trail *= Pass().TrailDecay;
    }

    const uint GroupContacts = min(gsContactCount, kMaxGroupContacts);
    for (uint Entry = 0u; Entry < GroupContacts; ++Entry)
    {
        const FWeatherContact Contact = Pass().Contacts.Get(gsContacts[Entry]);
        const bool  bGround = Contact.Surface == kSurfaceGround;
        const float Depth   = bGround ? kPuddleDepth : 0.0;

        float Now  = SubmergedChord(Contact.Position, Contact.Radius, Contact.SurfaceY + Depth, World);
        float Prev = SubmergedChord(Contact.PrevPosition, Contact.Radius, Contact.PrevSurfaceY + Depth, World);
        if (bGround)
        {
            // A foot can only push aside the water lying on the ground, however far the sphere sinks into it.
            Now  = min(Now, kPuddleDepth * 2.0);
            Prev = min(Prev, kPuddleDepth * 2.0);
        }

        // A body moving deep under the surface barely stirs it.
        const float Buried    = max(Contact.SurfaceY - (Contact.Position.y + Contact.Radius), 0.0);
        const float DepthFade = saturate(1.0 - Buried / max(2.0 * Contact.Radius, 0.1));
        const float Scale     = bOpenWater ? WaterRipple : 1.0;
        const float Moved     = (Now - Prev) * DepthFade;

        Height -= Moved * kDisplacementGain * Contact.RippleStrength * Scale;
        if (Contact.Surface == kSurfaceWater && bOpenWater)
        {
            Foam += abs(Moved) * kFoamGain * Contact.FoamStrength * WaterFoam;
        }
        // Only a foot planted on the ground prints, not one skimming over it mid stride.
        if (bGround)
        {
            const float Sunk = SubmergedChord(Contact.Position, Contact.Radius, Contact.SurfaceY + kPrintSink, World);
            Trail = max(Trail, saturate(Sunk / kPrintSink * 2.0 - 1.0) * Contact.TrailStrength);
        }
    }

    Height = clamp(Height, -kMaxHeight, kMaxHeight);
    Last   = clamp(Last, -kMaxHeight, kMaxHeight);
    Pass().Current[DTid.xy] = float4(Height, Last, saturate(Foam), saturate(Trail));
}
