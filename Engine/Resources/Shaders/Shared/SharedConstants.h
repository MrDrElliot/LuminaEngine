#pragma once

// Parsed by both MSVC and Slang, so only preprocessor directives and line comments; drift here corrupts memory.

#define MESHLET_MAX_VERTICES            64
#define MESHLET_MAX_TRIANGLES           64

// Discrete LODs or cluster hierarchy level slots per surface; a forced LOD plus one must still fit INSTANCE_FORCED_LOD_MASK.
#define MESHLET_MAX_LODS                12
// What files written before the slot count grew hold, and read back as.
#define MESHLET_LEGACY_MAX_LODS         6

// FMeshletDraw packs a mesh-global meshlet index into this many bits; past the bound it silently resolves the wrong meshlet.
#define MESHLET_DRAW_INDEX_BITS         20u

// The spare 12 bits record a cascade entry's deferring cascade, so the cull drops a defer past 1024 rather than wrap.
#define MESHLET_DEFER_DRAWID_BITS       10u
#define MESHLET_DEFER_MAX_DRAWID        1024u

// Mirrored by RHI::kMeshletCullGroupSize, which MeshData.h static_asserts against.
#define MESHLET_CULL_GROUP_SIZE         32

// The two VisBuffer phases share one cull view, so each tracks its appends apart and single-phase passes take MESHLET_SLICE_ALL.
#define MESHLET_SLICE_EARLY             0u
#define MESHLET_SLICE_LATE              1u
#define MESHLET_SLICE_ALL               2u
#define MESHLET_SLICE_COUNT             3u

// Y-fold axis, far below the 65535 Vulkan guarantees because the fold rounds group counts up to a multiple of it.
#define MAX_DISPATCH_AXIS               1024

// One grid per mesh, 21 bits per axis in two words; anchor plus offset stays below 2^24 so the decode is exact, which early-Z relies on.
#define MESH_POSITION_BITS              21
#define MESH_POSITION_MAX               0x1FFFFFu

// FMeshlet's last word, low bit first, is its vertex count, triangle count, LOD and whether its refs are 16-bit.
#define MESHLET_COUNT_BITS              7
#define MESHLET_COUNT_MASK              0x7Fu
#define MESHLET_TRIANGLE_COUNT_SHIFT    7u
#define MESHLET_LOD_BITS                4
#define MESHLET_LOD_MASK                0xFu
#define MESHLET_LOD_SHIFT               14u
#define MESHLET_SHORT_REFS_SHIFT        18u
// Set on a voxel splat meshlet, whose quads stand for a surface and so skip the material's opacity clip.
#define MESHLET_VOXEL_SHIFT             19u

// FSurfaceDesc::Flags. The cluster bit turns a view's pick from one slot into a slot range the per-meshlet cut then refines.
#define SURFACE_DESC_CLUSTER_LOD        1u
// Slot L holds voxel geometry when bit (SURFACE_DESC_VOXEL_SHIFT + L) is set, and its meshlets' parents are voxels when bit (SURFACE_DESC_VOXEL_PARENT_SHIFT + L) is.
#define SURFACE_DESC_VOXEL_SHIFT        4u
#define SURFACE_DESC_VOXEL_PARENT_SHIFT 16u

// FMeshletLODBounds::Flags, the per-meshlet form of the same two facts.
#define MESHLET_LOD_FLAG_VOXEL          1u
#define MESHLET_LOD_FLAG_VOXEL_PARENT   2u
// A parent error no tolerance accepts, so a root meshlet is always drawn once its own error is.
#define MESHLET_LOD_ERROR_INFINITE      3.0e38

// FMeshletHeader::Flags.
#define MESH_HEADER_FLAG_CLUSTER_LOD    1u

// FCullData::VirtualGeometryFlags, the project's runtime toggles.
#define VG_FLAG_VOXEL_FAR               1u
#define VG_FLAG_PROXIES                 2u

// FMeshletBlock bit above its count, set when the cull must draw every meshlet of the block without the cluster cut.
#define MESHLET_BLOCK_FORCE_ALL_BIT     (1u << 26u)

// The high bit of a VisBuffer word marks a far proxy, whose low bits hold its proxy draw index plus one.
#define VISBUFFER_PROXY_BIT             0x80000000u

// Which optional per-vertex streams a static mesh carries; an absent UV1 reads as UV0 and an absent color as white.
#define MESH_VERTEX_STREAM_UV1          1u
#define MESH_VERTEX_STREAM_COLOR        2u

// meshoptimizer's 8-bit SNORM cone, axis in bytes 0..2 and cutoff in byte 3, each decoded as x / 127.
#define MESHLET_CONE_SNORM_SCALE        127
// A cutoff of 127 decodes to 1.0, the value every cone reader already treats as no usable cone.
#define MESHLET_CONE_DISABLED           127

// Bone palette entries staged into groupshared memory (48 B each); a wider palette reads the arena direct.
#define SKIN_GROUP_PALETTE_BONES        64

// FGPUInstance::SurfaceDescIndex when the instance's LOD is fixed and no view may re-select it.
#define NO_SURFACE_DESC_INDEX           0xFFFFFFFFu

#define MATERIAL_CLASSIFY_TILE          8
// The deferred material pass runs one workgroup per classified tile, a thread per pixel.
#define MATERIAL_PIXEL_GROUP_SIZE       (MATERIAL_CLASSIFY_TILE * MATERIAL_CLASSIFY_TILE)
// Frames one streaming-feedback readback spans; the deferred material pass reports a rotating 1/N of its groups each frame.
#define STREAMING_FEEDBACK_WINDOW       4u
// Distinct deferred shaders one frame may bin, bounded by the R16_UINT slot image holding slot + 1.
#define MATERIAL_MAX_SLOTS              65535u
// The set a classify tile dedupes its slots in, twice the 64 a tile can hold so a probe stays short.
#define MATERIAL_TILE_CLAIM_BUCKETS     128u
// A pair past a tile's inline records is stored as its tile index, then its slot.
#define MATERIAL_OVERFLOW_PAIR_WORDS    2u
// Slots a tile records inline, past which its pairs go to the overflow list.
#define MATERIAL_TILE_SLOTS             4u
// A slot word plus a 64-bit pixel mask for each inline pair.
#define MATERIAL_TILE_RECORD            (MATERIAL_TILE_SLOTS * 3u)
#define MATERIAL_TILE_SPARSE_FLAG       0x80000000u
// A pair covering fewer pixels than this shades a lane per pixel, since a whole tile group would leave most lanes idle.
#define MATERIAL_SPARSE_PIXELS          16u

// Per-kind parameter caps. FMaterialUniforms holds every entry, and the GPU block keeps only each kind's used prefix.
#define MAX_SCALARS                     24
#define MAX_VECTORS                     24
#define MAX_TEXTURES                    24

// Collections one material may bind, each index packed into FMaterialHeader's Layout word.
#define MAX_MATERIAL_COLLECTIONS        2

// FMaterialCollectionUniforms layout, mirrored by FMaterialCollection in Common.slang.
#define MAX_COLLECTION_VECTORS          16
#define MAX_COLLECTION_SCALARS          16

// Slot 0 is a reserved all-zero collection, so a material binding none reads zeros without a sentinel.
#define MAX_PARAMETER_COLLECTIONS       64

// FMaterialHeader's Layout word, each kind's count up to its last nonzero entry, then the two collection indices.
#define MATERIAL_COUNT_MASK             31u
#define MATERIAL_SCALAR_COUNT_SHIFT     5u
#define MATERIAL_TEXTURE_COUNT_SHIFT    10u
#define MATERIAL_COLLECTION_SHIFT       16u
#define MATERIAL_COLLECTION_BITS        8u
#define MATERIAL_COLLECTION_MASK        255u

#define MAX_LIGHTS                      8192
#define MAX_SHADOWS                     256

// Light-function masks share one atlas of square tiles, so this many lights may carry one per frame.
#define LIGHT_FUNCTION_ATLAS_TILES      4u
#define LIGHT_FUNCTION_TILE_SIZE        512u
#define MAX_LIGHT_FUNCTIONS             (LIGHT_FUNCTION_ATLAS_TILES * LIGHT_FUNCTION_ATLAS_TILES)
// A light's atlas tile rides bits 8-15 of its flags, next to the LightFunction flag.
#define LIGHT_FUNCTION_SLOT_SHIFT       8u
#define LIGHT_FUNCTION_SLOT_MASK        255u
// A contact-shadowed local light's reach in centimeters rides bits 16-23 of its flags.
#define LIGHT_CONTACT_LENGTH_SHIFT      16u
#define LIGHT_CONTACT_LENGTH_MASK       255u
#define NUM_CASCADES                    4

// Hard cap on cull views, covering the camera, NUM_CASCADES, six per point light and one per spot.
#define MAX_CULL_VIEWS                  128

// LightCull gives each workgroup this block of clusters, so one bounding box pre-culls the lights for all of them.
#define CLUSTER_CULL_BLOCK_X            8
#define CLUSTER_CULL_BLOCK_Y            4
#define CLUSTER_CULL_BLOCK_Z            4

#define COL_R_SHIFT                     0
#define COL_G_SHIFT                     8
#define COL_B_SHIFT                     16
#define COL_A_SHIFT                     24
#define COL_A_MASK                      0xFF000000

#define PARTICLE_SIM_GROUP_SIZE         64u

// Emitter slots one workgroup can sort in groupshared; larger emitters take the multi-pass sort.
#define PARTICLE_SORT_CAPACITY          2048u
#define PARTICLE_SORT_INDEX_BITS        11u
#define PARTICLE_SORT_INDEX_MASK        0x7FFu
#define PARTICLE_SORT_DEPTH_BITS        21u
#define PARTICLE_SORT_THREADS           256

// The multi-pass sort keeps its keys in memory, and an emitter past this draws unsorted.
#define PARTICLE_GLOBAL_SORT_CAPACITY   65536u
#define PARTICLE_GLOBAL_SORT_INDEX_BITS 16u
#define PARTICLE_GLOBAL_SORT_INDEX_MASK 0xFFFFu
#define PARTICLE_GLOBAL_SORT_KEY_BITS   16u

// Events per list per frame an emitter can raise for its sub-emitters; extras are dropped.
#define PARTICLE_EVENT_CAPACITY         1024u
#define PARTICLE_EVENT_LISTS            3u

// Render layer bits, Default for anything that never chose a mask and Foreground for first-person geometry drawn over the world.
#define RENDER_LAYER_DEFAULT_MASK       1u
#define RENDER_LAYER_FOREGROUND_MASK    0x80000000u

// The low bits of an instance's LODAndThinning, its forced LOD plus one.
#define INSTANCE_FORCED_LOD_MASK        0xFu
// Set in an instance's LODAndThinning when its layers are anything but Default, so only those instances load their mask.
#define INSTANCE_CUSTOM_LAYERS_BIT      0x10u
// Where an instance's custom depth word sits in its LODAndThinning, nine bits below the thinning half.
#define INSTANCE_CUSTOM_DEPTH_SHIFT     7u
#define CUSTOM_DEPTH_WORD_MASK          0x1FFu
// Set in a custom depth word when the instance draws into custom depth, above its stencil byte.
#define CUSTOM_DEPTH_ENABLED_BIT        0x100u
#define CUSTOM_DEPTH_STENCIL_MASK       0xFFu
