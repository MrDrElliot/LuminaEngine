#include "Includes/SceneGlobals.slang"

// Must match FGPUParticle in ParticleSimulate.slang (64 bytes).
struct FGPUParticle
{
    float3  Position;
    float   Age;

    float3  Velocity;
    float   Lifetime;

    float4  Color;

    float   Size;
    float   Rotation;
    float   RotationSpeed;
    float   Seed;
};

// No pass-local set: the particle array is read by device address and the (tiny) per-emitter render
// params are inlined into the push constant.
// Must match EParticleFacingMode in ParticleSystem.h.
static const uint PARTICLE_FACING_CAMERA   = 0u;
static const uint PARTICLE_FACING_WORLD_XZ = 1u;
static const uint PARTICLE_FACING_VELOCITY = 2u;

struct FParticlePushConstants
{
    FGPUParticle* Particles;       // device address of the simulated particle array
    uint   TextureIndex;           // bindless 2D SRV (0 = default sprite)
    uint   FacingMode;             // EParticleFacingMode
    float4 Tint;                   // xyz tint rgb, w intensity multiplier
    float  VelocityStretch;        // extra length along velocity, in seconds of travel
    uint   SubUVColumns;           // sprite-sheet subdivisions; 1x1 = whole texture
    uint   SubUVRows;
    uint   AttrFloats;             // floats per particle in the attribute buffer
    float* Attributes;             // declared-attribute buffer, null when the emitter has none
    int    AttrSlotSizeScaleX;     // -1 when the stack did not declare it
    int    AttrSlotSizeScaleY;
    int    AttrSlotPrevPosX;       // written by the Trail module; -1 without one
    int    AttrSlotPrevPosY;
    int    AttrSlotPrevPosZ;
    uint   Pad1;
};
FParticlePushConstants* Pass() { return GetPassArgs<FParticlePushConstants>(); }

// Reads a renderer-consumed attribute, or Fallback when the module stack never declared it. The slot is
// resolved at compile time by FParticleCompiler and passed in, which is how this shared (non-generated)
// shader reads a layout it cannot know statically.
float ReadParticleAttr(uint ParticleIndex, int Slot, float Fallback)
{
    if (Slot < 0)
    {
        return Fallback;
    }
    return Pass().Attributes[ParticleIndex * Pass().AttrFloats + (uint)Slot];
}

static const float2 QuadPositions[6] =
{
    float2(-1.0, -1.0),
    float2( 1.0, -1.0),
    float2( 1.0,  1.0),

    float2(-1.0, -1.0),
    float2( 1.0,  1.0),
    float2(-1.0,  1.0)
};

static const float2 QuadUVs[6] =
{
    float2(0.0, 0.0),
    float2(1.0, 0.0),
    float2(1.0, 1.0),

    float2(0.0, 0.0),
    float2(1.0, 1.0),
    float2(0.0, 1.0)
};

struct VertexOutput
{
    float4                  Color        : PARTICLE_COLOR;
    float2                  UV           : TEXCOORD0;
    nointerpolation uint    TextureIndex : TEXTURE_INDEX;
    float4                  Position     : SV_Position;
};

[shader("vertex")]
VertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID)
{
    uint ParticleIndex = VertexIndex / 6;
    uint QuadIndex     = VertexIndex % 6;

    FGPUParticle P = Pass().Particles[ParticleIndex];

    VertexOutput Out;

    // Dead particles collapse to a degenerate clip-culled point.
    bool bAlive = (P.Lifetime > 0.0) && (P.Age < P.Lifetime) && (P.Size > 0.0);
    if (!bAlive)
    {
        Out.Position     = float4(2.0, 2.0, 2.0, 1.0);
        Out.Color        = float4(0.0, 0.0, 0.0, 0.0);
        Out.UV           = float2(0.0, 0.0);
        Out.TextureIndex = 0;
        return Out;
    }

    // Fetched once and reused for the final clip transform below (avoids a second matrix load).
    float4x4 ViewMatrix = GetCameraView();

    const float3 CameraRight =  float3(ViewMatrix[0][0], ViewMatrix[0][1], ViewMatrix[0][2]);
    const float3 CameraUp    = -float3(ViewMatrix[1][0], ViewMatrix[1][1], ViewMatrix[1][2]);

    float3 BillboardRight;
    float3 BillboardUp;
    // Every mode but Trail keeps the quad centred on the particle; Trail moves it to the midpoint of the
    // segment it spans, so the centre has to be a variable rather than folded into the final add.
    float3 QuadCenter = P.Position;
    // Set once a mode has actually pinned the quad's axes to a direction. Rotation is applied in the
    // billboard's own plane, which reads correctly for a camera-facing sprite but would swing a directional
    // quad off the very axis the mode exists to align it to -- so those modes ignore it. The fallbacks
    // below (no velocity, no trail yet) are genuinely camera-facing and keep rotating as normal.
    bool bDirectional = false;
    // Non-uniform size: multipliers on the simulated size, so an emitter that declares neither behaves
    // exactly as before and one that declares only X stretches on that axis alone.
    float2 Extent = float2(P.Size, P.Size);
    Extent.x *= ReadParticleAttr(ParticleIndex, Pass().AttrSlotSizeScaleX, 1.0);
    Extent.y *= ReadParticleAttr(ParticleIndex, Pass().AttrSlotSizeScaleY, 1.0);

    // A declared PrevPos attribute means the stack has a Trail module, and that is the whole signal: a
    // trail is not one more way to orient a sprite, it is a different primitive, so it takes precedence
    // over FacingMode rather than being a value of it. Requiring the module AND a matching enum meant
    // adding "Trail" and seeing nothing change, with nothing on screen to say why.
    if (Pass().AttrSlotPrevPosX >= 0)
    {
        // The lagging position the Trail module records.
        const float3 Prev = float3(ReadParticleAttr(ParticleIndex, Pass().AttrSlotPrevPosX, P.Position.x),
                                   ReadParticleAttr(ParticleIndex, Pass().AttrSlotPrevPosY, P.Position.y),
                                   ReadParticleAttr(ParticleIndex, Pass().AttrSlotPrevPosZ, P.Position.z));

        const float3 Delta = P.Position - Prev;
        const float  Len   = length(Delta);
        // A particle that has not moved yet has no segment to span, so it draws as a plain camera-facing
        // sprite for that frame rather than collapsing to a degenerate quad.
        if (Len > 1e-5)
        {
            const float3 Dir = Delta / Len;
            // Row 2 of the view matrix, matching CameraRight/CameraUp above.
            const float3 CameraForward = float3(ViewMatrix[2][0], ViewMatrix[2][1], ViewMatrix[2][2]);
            float3 Side = cross(Dir, CameraForward);
            const float SideLen = length(Side);
            Side = (SideLen > 1e-5) ? (Side / SideLen) : CameraRight;

            BillboardUp    = Dir;
            BillboardRight = Side;
            // The quad SPANS the travelled segment: half its length along the segment, recentred on the
            // segment's midpoint. The sprite's own extent is kept on top so the streak has a cap at each
            // end rather than being cut off square at the particle.
            QuadCenter   = Prev + Dir * (Len * 0.5);
            Extent.y    += Len * 0.5;
            bDirectional = true;
        }
        else
        {
            BillboardRight = CameraRight;
            BillboardUp    = CameraUp;
        }
    }
    else if (Pass().FacingMode == PARTICLE_FACING_VELOCITY)
    {
        // Up follows the direction of travel, right stays perpendicular to it AND to the view direction,
        // so the quad keeps its width on screen however the particle is moving. A particle with no
        // velocity has no direction to align to, so it falls back to a camera-facing quad rather than
        // collapsing to a degenerate one.
        const float Speed = length(P.Velocity);
        if (Speed > 1e-5)
        {
            const float3 Dir = P.Velocity / Speed;
            // Row 2 of the view matrix, matching how CameraRight/CameraUp are taken above. Reading a
            // column here instead would give an unrelated axis and skew every velocity-aligned quad.
            const float3 CameraForward = float3(ViewMatrix[2][0], ViewMatrix[2][1], ViewMatrix[2][2]);
            float3 Side = cross(Dir, CameraForward);
            const float SideLen = length(Side);
            Side = (SideLen > 1e-5) ? (Side / SideLen) : CameraRight;

            BillboardUp    = Dir;
            BillboardRight = Side;
            // Stretch measured in seconds of travel, so a fixed value reads consistently across speeds.
            Extent.y += Speed * Pass().VelocityStretch;
            bDirectional = true;
        }
        else
        {
            BillboardRight = CameraRight;
            BillboardUp    = CameraUp;
        }
    }
    else if (Pass().FacingMode == PARTICLE_FACING_WORLD_XZ)
    {
        BillboardRight = float3(1.0, 0.0, 0.0);
        BillboardUp    = float3(0.0, 0.0, 1.0);
    }
    else
    {
        BillboardRight = CameraRight;
        BillboardUp    = CameraUp;
    }

    // Corner offset in the billboard's local 2D frame, rotated by particle rotation.
    float2 Local = QuadPositions[QuadIndex] * Extent;
    float  Angle = bDirectional ? 0.0 : P.Rotation;
    float  s     = sin(Angle);
    float  c     = cos(Angle);
    float2 Rotated = float2(Local.x * c - Local.y * s,
                            Local.x * s + Local.y * c);

    float3 WorldPos = QuadCenter
                    + BillboardRight * Rotated.x
                    + BillboardUp    * Rotated.y;

    // Sub-UV: remap the quad's 0..1 UV into one cell of the sheet. The frame advances with age, so it
    // needs no per-particle storage. 1x1 leaves the UV untouched.
    float2 UV = QuadUVs[QuadIndex];
    const uint FrameCount = max(Pass().SubUVColumns * Pass().SubUVRows, 1u);
    if (FrameCount > 1u)
    {
        const float LifeRatio = saturate(P.Age / max(P.Lifetime, 1e-4));
        // Clamped off the last frame: at LifeRatio == 1 the floor would land one cell past the sheet.
        const uint  Frame     = min((uint)(LifeRatio * (float)FrameCount), FrameCount - 1u);
        const float2 CellSize = float2(1.0 / (float)Pass().SubUVColumns, 1.0 / (float)Pass().SubUVRows);
        const float2 CellIdx  = float2((float)(Frame % Pass().SubUVColumns), (float)(Frame / Pass().SubUVColumns));
        UV = (CellIdx + UV) * CellSize;
    }

    Out.UV           = UV;
    Out.Color        = P.Color * float4(Pass().Tint.rgb * Pass().Tint.w, 1.0);
    Out.TextureIndex = Pass().TextureIndex;
    Out.Position     = mul(GetCameraProjection(), mul(ViewMatrix, float4(WorldPos, 1.0)));
    return Out;
}
