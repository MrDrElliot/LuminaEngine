#pragma once
#include "Platform/GenericPlatform.h"

#define PACKAGE_FILE_TAG			0x9E2A83C1

#define PREPROCESSOR_ENUM_PROTECT(a) ((unsigned int)(a))

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

	// FMeshResource serializes its baked signed distance field volume.
	MESH_DISTANCE_FIELD,

	// FTextureResource::FDescription serializes LayerCount, and its mips are stored layer-major
	// (Mips[Layer * NumMips + Mip]). Older files are single-layer, so LayerCount defaults to 1.
	TEXTURE_ARRAY_LAYERS,

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

