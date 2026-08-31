#pragma once

// Parsed by BOTH MSVC and Slang: preprocessor directives and // comments ONLY. Everything here sizes a
// buffer or is packed into a field both sides decode, so drift corrupts memory instead of misbehaving.

#define MESHLET_MAX_VERTICES            64
#define MESHLET_MAX_TRIANGLES           64

// LOD 0 is full detail. Sloppy LODs (4-5) can hole, which reads as a shadow light-leak, so casters
// cap lower -- except past ShadowCoarseLODDistance, where one cascade texel keeps the holes sub-texel.
#define MESHLET_MAX_LODS                6
#define MESHLET_MAX_SHADOW_LOD          3
#define MESHLET_MAX_COARSE_SHADOW_LOD   5

// FMeshletDraw packs a mesh-global meshlet index into this many bits and spends the rest on the frame
// tag. Past the bound the index wraps and silently resolves the wrong meshlet.
#define MESHLET_DRAW_INDEX_BITS         20u

// The remaining 12 bits of the packed word are spare, and for a cascade entry record which cascade
// deferred it. The cull drops a defer past 1024 rather than wrapping into the cascade field.
#define MESHLET_DEFER_DRAWID_BITS       10u
#define MESHLET_DEFER_MAX_DRAWID        1024u

// Not a device preference: the block list is laid out CPU-side in units of this and the cull workgroup
// is declared from it. RHI::kMeshletCullGroupSize mirrors it and MeshData.h static_asserts the pair.
#define MESHLET_CULL_GROUP_SIZE         32

// Which part of a bucket's draw region a geometry pass rasterizes. The two VisBuffer phases share one
// cull view, so each phase's appends are tracked separately; every single-phase pass takes ALL, which
// by then is final. Sized per slice: bases, counts, sub-draw counts and indirect args.
#define MESHLET_SLICE_EARLY             0u
#define MESHLET_SLICE_LATE              1u
#define MESHLET_SLICE_ALL               2u
#define MESHLET_SLICE_COUNT             3u

// Y-fold axis, far below the 65535 Vulkan guarantees because the fold rounds group counts up to a multiple of it.
#define MAX_DISPATCH_AXIS               1024

// DXR2 COMPRESSED1: three 16-bit offsets from a signed 24-bit anchor on a shared power-of-two exponent.
// Anchor + Offset < 2^24, so the decode is bit-identical everywhere -- load-bearing for early-Z.
#define MESHLET_POSITION_MAX            65535
#define MESHLET_ANCHOR_MAX              8388607
#define MESHLET_ANCHOR_MASK             0x00FFFFFFu
#define MESHLET_ANCHOR_SIGN             0x00800000u
#define MESHLET_EXPONENT_SHIFT          24u

// meshoptimizer's 8-bit SNORM cone, axis in bytes 0..2 and cutoff in byte 3, each decoded as x / 127.
#define MESHLET_CONE_SNORM_SCALE        127
// A cutoff of 127 decodes to 1.0, the value every cone reader already treats as no usable cone.
#define MESHLET_CONE_DISABLED           127

// Bone palette entries staged into groupshared memory (48 B each); a wider palette reads the arena direct.
#define SKIN_GROUP_PALETTE_BONES        64

// FGPUInstance::SurfaceDescIndex when the instance's LOD is fixed and no view may re-select it.
#define NO_SURFACE_DESC_INDEX           0xFFFFFFFFu

#define MATERIAL_CLASSIFY_TILE          8
#define MATERIAL_PIXEL_GROUP_SIZE       64
// Distinct deferred shaders one frame may bin. A backstop, not a knob; costs are linear in the live count.
#define MATERIAL_MAX_SLOTS              1024u

// FMaterialUniforms layout. Changing one side reinterprets every field after it.
#define MAX_SCALARS                     24
#define MAX_VECTORS                     24
#define MAX_TEXTURES                    24

// Collections one material may bind. Their indices sit in words FMaterialUniforms already reserved.
#define MAX_MATERIAL_COLLECTIONS        2

// FMaterialCollectionUniforms layout, mirrored by FMaterialCollection in Common.slang.
#define MAX_COLLECTION_VECTORS          16
#define MAX_COLLECTION_SCALARS          16

// Slot 0 is a reserved all-zero collection, so a material binding none reads zeros without a sentinel.
#define MAX_PARAMETER_COLLECTIONS       64

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

// Emitter slots one workgroup can sort in groupshared; a larger emitter draws unsorted and uncompacted.
#define PARTICLE_SORT_CAPACITY          2048u
#define PARTICLE_SORT_INDEX_BITS        11u
#define PARTICLE_SORT_INDEX_MASK        0x7FFu
#define PARTICLE_SORT_DEPTH_BITS        21u
#define PARTICLE_SORT_THREADS           256
