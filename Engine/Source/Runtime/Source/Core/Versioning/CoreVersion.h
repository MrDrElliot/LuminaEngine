#pragma once
#include "Platform/GenericPlatform.h"

#define PACKAGE_FILE_TAG			0x9E2A83C1

#define PREPROCESSOR_ENUM_PROTECT(a) ((unsigned int)(a))

// Package wire format entries are positional, so removing one renumbers every later entry and silently changes what old files mean.
enum class ELuminaEngineVersion : uint32
{
	INITIAL_VERSION = 1000,

	// CAnimationGraph serializes NumSyncGroups (phase-matched blend sync groups).
	ANIM_GRAPH_SYNC_GROUPS,

	// CAnimationGraph serializes BytecodeVersion (stale programs refused instead of misparsed).
	ANIM_GRAPH_BYTECODE_VERSION,

	// None serializes as the empty string, where older files stored the "NAME_None" display rendering.
	FNAME_NONE_EMPTY_STRING,

	// Texture descriptions serialize LayerCount with mips stored layer-major, and older files default to 1 layer.
	TEXTURE_ARRAY_LAYERS,

	// FAnimationResource serializes authored float curves, and CAnimationGraph its curve tables.
	ANIM_CURVES,

	// CPrefab writes a variant flag before its registry, and a variant stores a delta against its parent.
	PREFAB_VARIANTS,

	// Packages can carry a bulk-data region located by a trailer at EOF, and older files store every mip inline.
	PACKAGE_BULK_DATA,

	// Older meshes store a TexelFactor of 0 and fall back to a bounding sphere estimate, since source UVs are not serialized.
	MESH_SURFACE_TEXEL_FACTOR,

	// CAnimationGraph serializes SlotNames, the montage slot table its EvalSlot opcodes index.
	ANIM_GRAPH_MONTAGE_SLOTS,

	// CAnimationGraph serializes NumObjectRegisters, the register file its object dataflow uses.
	ANIM_GRAPH_OBJECT_PARAMETERS,

	// CTexture keeps its imported file's bytes (bulk region, editor-only), so cook settings stay absolute.
	TEXTURE_SOURCE_FILE,

	// FMeshletData serializes per-meshlet bone palettes; a skinned vertex's JointIndices address one.
	MESHLET_BONE_PALETTES,

	// FAnimationNotify/FAnimationNotifyState carry an instanced notify, so one can run its own code.
	ANIM_NOTIFY_OBJECTS,

	// FAnimationResource carries a uniformly resampled, quantized copy of its channels (re-import to gain one).
	ANIM_COMPRESSED_TRACKS,

	// FAnimationResource no longer writes its raw channels; older files still read them and compress on load.
	ANIM_CHANNELS_DROPPED,

	// FAnimGraphStateMachine serializes per-state clock ranges; FAnimGraphTransition its condition source.
	ANIM_GRAPH_STATE_CLOCKS,

	// Clock ranges index a serialized ClockSlots list, which excludes a nested machine's bookkeeping.
	ANIM_GRAPH_STATE_CLOCK_LIST,

	// CAnimationGraph serializes the smoothing record counts its Inertialization / Dead Blending nodes use.
	ANIM_GRAPH_INERTIALIZATION_NODES,

	// FAnimGraphTransition carries a list of condition terms; states carry their clip Finished register.
	ANIM_GRAPH_TRANSITION_TERMS,

	// CAnimationGraph serializes the named pose snapshot slots its snapshot opcodes address.
	ANIM_GRAPH_POSE_SNAPSHOTS,

	// SEntityScriptComponent length-prefixes each script, so an unresolvable one is skipped, not fatal.
	ENTITY_SCRIPT_LENGTH_PREFIX,

	// FMeshletCone is a 4-byte SNORM8 axis + cutoff with no apex. Older files hold floats and are quantized on load.
	MESHLET_CONE_SNORM8,

	// FAnimationResource names the notify track whose entries are its sync markers.
	ANIM_SYNC_TRACK,

	// Packages carry a name table, and a property header spells its kind in one byte with no dead offset.
	PACKAGE_NAME_TABLE,

	// FTextureResource::FDescription carries the swizzle a single-channel format is sampled through.
	TEXTURE_SWIZZLE,

	// An entity stores its parent and sibling index, replacing the removed FRelationshipComponent's raw links.
	REGISTRY_PARENT_LINKS,

	// FGeometrySurface stores each LOD's measured error; older meshes load LOD 0 only until reimported.
	MESH_LOD_ERROR,

	// One position grid per mesh, static vertices stored once behind per-meshlet refs, and skinned vertices on the same grid.
	MESHLET_SHARED_VERTICES,

	// An entity script whose class was not loaded is written after the live ones with the names and objects its bytes refer to.
	PORTABLE_HELD_SCRIPTS,

	// A soft import lists the asset bundles that reference it.
	ASSET_BUNDLES,

	// A held value carries the entity remaps that copies made while its type was gone.
	HELD_VALUE_ENTITY_REMAP,
	// An array records its element kind beside the element size, so a retyped element converts or is skipped instead of misread.
	ARRAY_ELEMENT_KIND,

	// An export names its class's type package, so a build without the editor can tell editor data from a missing class.
	EXPORT_CLASS_PACKAGE,

	// Surfaces hold twelve LOD slots with parent errors and flags, and FMeshletData carries cluster hierarchy bounds per meshlet.
	MESH_CLUSTER_LOD,

	// A mesh resource carries an optional baked impostor after its distance field.
	MESH_IMPOSTOR,

	// An impostor records whether its atlases hold BC7 blocks, and older ones are RGBA8.
	MESH_IMPOSTOR_BC7,

	AUTOMATIC_VERSION_PLUS_ONE,
	AUTOMATIC_VERSION = AUTOMATIC_VERSION_PLUS_ONE - 1
};


struct FPackageFileVersion
{
	FPackageFileVersion(ELuminaEngineVersion EngineVersion) noexcept
	: FileVersion(static_cast<int32>(EngineVersion)) {}
	
	bool operator >=(ELuminaEngineVersion Version) const
	{
		return FileVersion >= static_cast<int32>(Version);
	}
	
	int32		FileVersion = 0;
};

#define VER_LATEST_ENGINE           PREPROCESSOR_ENUM_PROTECT(ELuminaEngineVersion::AUTOMATIC_VERSION)

namespace Lumina::Global
{
    extern RUNTIME_API const FPackageFileVersion GPackageFileLuminaVersion;
}

