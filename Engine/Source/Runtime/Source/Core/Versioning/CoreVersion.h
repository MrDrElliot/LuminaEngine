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

extern const FPackageFileVersion GPackageFileLuminaVersion;

