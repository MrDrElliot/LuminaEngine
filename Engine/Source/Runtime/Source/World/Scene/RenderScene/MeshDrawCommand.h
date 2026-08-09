#pragma once
#include "Containers/Array.h"
#include "Platform/GenericPlatform.h"
#include <Core/Math/Hash/Hash.h>
#include <Renderer/RHIFwd.h>

namespace Lumina
{
	class CMaterialInterface;
	
	struct FRenderMaterialShaders
	{
		const FShaderEntry* VertexShader = nullptr;
		const FShaderEntry* PixelShader  = nullptr;
	};

	// Keyed by what forces a distinct PIPELINE, not by material identity. Two materials that compile to the
	// same shaders share one batch and therefore one draw -- which is every material without World Position
	// Offset, since their geometry stages are byte-identical and the shader library hands out one entry for
	// identical bytecode.
	//
	// The material is carried per INSTANCE (FGPUInstance::MaterialIndex) and per deferred slot
	// (FBatch::DeferredMaterials, which unions across every material in the batch), so nothing downstream
	// needs a batch to be one material.
	struct FDrawBatchKey
	{
		const FShaderEntry* VisBufferMeshShader        = nullptr;
		const FShaderEntry* VisBufferMeshShaderMasked  = nullptr;
		const FShaderEntry* MaskedVisBufferPixelShader = nullptr;
		const FShaderEntry* MeshShaderBase             = nullptr;
		const FShaderEntry* MeshShaderShadow           = nullptr;
		const FShaderEntry* PixelShader                = nullptr;
		const FShaderEntry* MomentPixelShader          = nullptr;

		uint32 bTranslucent : 1;
		uint32 bMasked : 1;
		uint32 bAdditive : 1;
		uint32 bTwoSided : 1;

		bool operator == (const FDrawBatchKey& Key) const
		{
			return VisBufferMeshShader        == Key.VisBufferMeshShader
				&& VisBufferMeshShaderMasked  == Key.VisBufferMeshShaderMasked
				&& MaskedVisBufferPixelShader == Key.MaskedVisBufferPixelShader
				&& MeshShaderBase             == Key.MeshShaderBase
				&& MeshShaderShadow           == Key.MeshShaderShadow
				&& PixelShader                == Key.PixelShader
				&& MomentPixelShader          == Key.MomentPixelShader
				&& bTranslucent == Key.bTranslucent
				&& bMasked      == Key.bMasked
				&& bAdditive    == Key.bAdditive
				&& bTwoSided    == Key.bTwoSided;
		}
	};

	static uint64 GetTypeHash(const FDrawBatchKey& K)
	{
		size_t Seed = 0;
		for (const FShaderEntry* Entry : { K.VisBufferMeshShader, K.VisBufferMeshShaderMasked,
										   K.MaskedVisBufferPixelShader, K.MeshShaderBase,
										   K.MeshShaderShadow, K.PixelShader, K.MomentPixelShader })
		{
			Hash::HashCombine(Seed, (uint64)(uintptr_t)Entry);
		}
		Hash::HashCombine(Seed, K.bTranslucent);
		Hash::HashCombine(Seed, K.bMasked);
		Hash::HashCombine(Seed, K.bAdditive);
		Hash::HashCombine(Seed, K.bTwoSided);
		return Seed;
	}

	struct FMeshDrawCommand
	{
		const FShaderEntry*					PixelShader  = nullptr;
		const FShaderEntry*					VertexShader = nullptr;
		const FShaderEntry*					MeshShaderShadow = nullptr;          // shadow depth (position-only out)
		const FShaderEntry*					MeshShaderBase = nullptr;            // translucent / additive (full interpolants)
		const FShaderEntry*					VisBufferMeshShader = nullptr;       // VisBuffer geometry, opaque (position-only out)
		const FShaderEntry*					VisBufferMeshShaderMasked = nullptr; // VisBuffer geometry, masked (full interpolants)
		const FShaderEntry*					MaskedVisBufferPixelShader = nullptr;// masked-only PS: opacity clip before VisID/depth
		const FShaderEntry*					MomentPixelShader = nullptr;         // MBOIT pass 1: opacity-only moment accumulation
		uint32                      		IndirectDrawOffset = 0;
		uint32                      		DrawCount = 0;
		uint32                      		bTranslucent : 1;
		uint32                      		bMasked : 1;
		uint32                      		bAdditive : 1;
		uint32                      		bTwoSided : 1;        // two-sided material: VisBuffer disables back-face cull
		uint32                      		bAnySkinned : 1;      // batch has >=1 skinned instance (SPEC_SKINNED variant select)
		uint32                      		bAnyStatic  : 1;      // batch has >=1 static instance; both set => mixed => dynamic
	};
}
