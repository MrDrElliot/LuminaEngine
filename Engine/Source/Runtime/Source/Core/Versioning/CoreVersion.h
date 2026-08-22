#pragma once
#include "Platform/GenericPlatform.h"

#define PACKAGE_FILE_TAG			0x9E2A83C1

#define PREPROCESSOR_ENUM_PROTECT(a) ((unsigned int)(a))

// Package wire format. Entries are POSITIONAL: the value is what gets stamped into every saved package,
// so removing one renumbers every entry after it and changes what already-written files mean. Only do
// that when the content those files hold is being regenerated wholesale -- a package stamped higher than
// AUTOMATIC_VERSION is refused outright by LoadPackage, which is loud, but the reverse is silent.
enum class ELuminaEngineVersion : uint32
{
	INITIAL_VERSION = 1000,

	// CAnimationGraph serializes NumSyncGroups (phase-matched blend sync groups).
	ANIM_GRAPH_SYNC_GROUPS,

	// CAnimationGraph serializes BytecodeVersion (stale programs refused instead of misparsed).
	ANIM_GRAPH_BYTECODE_VERSION,

	// FName wire format: None serializes as the empty string. Older files stored the "NAME_None"
	// display rendering, which round-tripped into a real name whose IsNone() was false.
	FNAME_NONE_EMPTY_STRING,

	// FTextureResource::FDescription serializes LayerCount, and its mips are stored layer-major
	// (Mips[Layer * NumMips + Mip]). Older files are single-layer, so LayerCount defaults to 1.
	TEXTURE_ARRAY_LAYERS,

	// FAnimationResource serializes authored float curves, and CAnimationGraph its curve tables.
	ANIM_CURVES,

	// CPrefab writes a variant flag before its registry. A variant serializes its DELTA against
	// ParentPrefab there instead of resolved data; older files are always root prefabs.
	PREFAB_VARIANTS,

	// Packages can carry a bulk-data region: raw bytes appended after the compressed container and
	// located by a fixed trailer at EOF, addressed by FBulkDataRef offsets stored inline in exports.
	// FTextureResource uses it to hold the mips above its inline tail, so a texture no longer has to
	// be fully resident to be loaded. Older files have no trailer and store every mip inline.
	PACKAGE_BULK_DATA,

	// FGeometrySurface serializes TexelFactor (world size of one UV tile), which texture streaming uses
	// to turn a distance into a required resolution. Older meshes store 0 and fall back to a bounding-
	// sphere estimate; it cannot be recomputed at load because the source UVs are not serialized.
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

extern RUNTIME_API const FPackageFileVersion GPackageFileLuminaVersion;

