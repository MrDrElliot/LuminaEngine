#pragma once

// The single definition of every constant C++ and Slang both depend on. Anything here sizes a GPU
// buffer, bounds an array both sides index, or is packed into a field both sides decode -- so drift
// corrupts memory instead of misbehaving visibly, and a `// must match` comment is not enough.
//
// Parsed by MSVC and by Slang, so preprocessor directives and `//` comments ONLY: no types, no
// constexpr, no expressions either language would reject. Both sides include it by the same path.
// C++ reaches it through the Shaders module's include path (Runtime.Build.cs); Slang resolves it
// against the /Engine/Resources/Shaders search root.
//
// Literal spellings are load-bearing on the Slang side -- several of these appear in mesh-shader
// output declarations and groupshared array bounds. Don't add or drop a `u` suffix casually.

// 64 verts / 124 tris = AMD/NV mesh-shader sweet spot, and satisfies meshopt's limits.
#define MESHLET_MAX_VERTICES            64
#define MESHLET_MAX_TRIANGLES           124

// LOD 0 is full detail. Sloppy LODs (4-5) can hole, which reads as a shadow light-leak, so casters
// cap lower -- except past ShadowCoarseLODDistance, where one cascade texel keeps the holes sub-texel.
#define MESHLET_MAX_LODS                6
#define MESHLET_MAX_SHADOW_LOD          3
#define MESHLET_MAX_COARSE_SHADOW_LOD   5

// FMeshletDraw packs a mesh-global meshlet index into this many bits and spends the rest on the frame
// tag. Past the bound the index wraps and silently resolves the wrong meshlet.
#define MESHLET_DRAW_INDEX_BITS         20u

// FMeshletDeferred spends the remaining 12 bits of its packed word on the batch the entry re-emits into
// and, for a cascade entry, which cascade deferred it. A draw is a PSO bucket -- tens in practice -- so
// 1024 is generous; the cull drops a defer past it rather than wrapping into the cascade field.
#define MESHLET_DEFER_DRAWID_BITS       10u
#define MESHLET_DEFER_MAX_DRAWID        1024u

// One task (amplification) workgroup covers this many meshlets. Not a device preference the engine can
// follow: the block list is laid out CPU-side in units of this, and the payload array is declared from
// it. RHI::kTaskWorkGroupSize mirrors it and MeshData.h static_asserts the pair.
#define MESHLET_TASK_GROUP_SIZE         32

// Per-axis compute workgroup cap a grid folds across. Vulkan only guarantees 65535 for
// maxComputeWorkGroupCount, so any dispatch whose domain can exceed that folds into Y -- and the shader
// must undo the fold with GroupID.y * this * its group size. Shared because the two halves of that fold
// are written in different languages and a mismatch silently duplicates or drops whole rows of work.
#define MAX_DISPATCH_AXIS               65535

// Meshlet vertex position quantization. A position is three 16-bit unsigned offsets from a signed
// 24-bit per-meshlet anchor, all scaled by one shared power-of-two exponent -- the DXR2 COMPRESSED1
// encoding, so the vertex stream feeds a ray-tracing BLAS build without conversion.
//
// Error is relative to the MESHLET's extent, not the model origin, so for a ~124-triangle patch this
// is finer than float32's absolute mantissa spacing once the mesh sits any distance from its origin.
//
// Decode is (Anchor + Offset) converted to float and multiplied by an exact power of two. Anchor +
// Offset never exceeds 8388607 + 65535 < 2^24, so the int->float conversion is exact and the multiply
// only touches the exponent: every pass decodes bit-identical positions. That is load-bearing -- the
// VisBuffer geometry pass and DeferredMaterial reconstruct the same triangle independently, and the
// depth prepass shares positions with the geometry pass for early-Z.
#define MESHLET_POSITION_MAX            65535
#define MESHLET_ANCHOR_MAX              8388607
#define MESHLET_ANCHOR_MASK             0x00FFFFFFu
#define MESHLET_ANCHOR_SIGN             0x00800000u
#define MESHLET_EXPONENT_SHIFT          24u

// FGPUInstance::SurfaceDescIndex when the instance's LOD is fixed and no view may re-select it.
#define NO_SURFACE_DESC_INDEX           0xFFFFFFFFu

// Square tile edge in pixels: coarse enough to keep the tile count and per-tile bitmask small, fine
// enough that over-inclusion (a tile binned for a material that only clips its corner) stays cheap.
// The slot cap bounds that bitmask's width and the compacted tile list's per-slot stride.
#define MATERIAL_TILE_SIZE_PX           16u
#define MATERIAL_MAX_SLOTS              64u

// FMaterialUniforms layout. Changing one side reinterprets every field after it.
#define MAX_SCALARS                     24
#define MAX_VECTORS                     24
#define MAX_TEXTURES                    24

#define MAX_LIGHTS                      8192
#define MAX_SHADOWS                     256
#define NUM_CASCADES                    4

// Hard cap on cull views: camera + NUM_CASCADES + 6/point + 1/spot.
#define MAX_CULL_VIEWS                  128

// The cluster light list packs two 13-bit light indices per uint.
#define LIGHT_INDEX_MASK                0x1FFFu
#define LIGHTS_PER_UINT                 2
#define LIGHTS_PER_CLUSTER              100

#define COL_R_SHIFT                     0
#define COL_G_SHIFT                     8
#define COL_B_SHIFT                     16
#define COL_A_SHIFT                     24
#define COL_A_MASK                      0xFF000000
