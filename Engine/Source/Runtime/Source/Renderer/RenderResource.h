#pragma once

#include "Format.h"
#include "Core/LuminaMacros.h"

enum class EFormatKind : uint8
{
	Integer,
	Normalized,
	Float,
	DepthStencil
};

struct RUNTIME_API FFormatInfo
{
	EFormat Format;
	const char* Name;
	uint8 BytesPerBlock;
	uint8 BlockSize;
	EFormatKind Kind;
	uint8 bHasRed : 1;
	uint8 bHasGreen : 1;
	uint8 bHasBlue : 1;
	uint8 bHasAlpha : 1;
	uint8 bHasDepth : 1;
	uint8 bHasStencil : 1;
	uint8 bIsSigned : 1;
	uint8 bIsSRGB : 1;
};

namespace Lumina::RHI::Format
{
	RUNTIME_API const FFormatInfo& Info(EFormat format);
	RUNTIME_API uint8 BytesPerBlock(EFormat Format);
}

// Serialized in the shader cache (.lsc); keep values stable or bump SHADER_CACHE_VERSION.
enum class ERHIShaderType : uint8
{
	None     = 0,
	Vertex   = 1,
	Fragment = 2,
	Compute  = 3,
	Geometry = 4,
	Mesh     = 5,
	Task     = 6,
};
