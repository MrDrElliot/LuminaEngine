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


	struct FDrawBatchKey
	{
		uint64 MaterialID;

		uint32 bTranslucent : 1;
		uint32 bMasked : 1;
		uint32 bAdditive : 1;
		uint32 bTwoSided : 1;

		bool operator == (const FDrawBatchKey& Key) const
		{
			return MaterialID == Key.MaterialID
				&& bTranslucent == Key.bTranslucent
				&& bMasked == Key.bMasked
				&& bAdditive == Key.bAdditive
				&& bTwoSided == Key.bTwoSided;
		}
	};

	static uint64 GetTypeHash(const FDrawBatchKey& K)
	{
		size_t Seed = 0;
		Hash::HashCombine(Seed, K.MaterialID);
		Hash::HashCombine(Seed, K.bTranslucent);
		Hash::HashCombine(Seed, K.bMasked);
		Hash::HashCombine(Seed, K.bAdditive);
		Hash::HashCombine(Seed, K.bTwoSided);
		return Seed;
	}

	// All data needed for one mesh draw call; cached in the scene. Shader entries are
	// library-owned (immortal), so a deleted material asset can't dangle the render thread.
	struct FMeshDrawCommand
	{
		const FShaderEntry*					VertexShader = nullptr;
		const FShaderEntry*					PixelShader  = nullptr;
		const FShaderEntry*					MeshShader   = nullptr;
		const FShaderEntry*					VisBufferMeshShader   = nullptr;   // VisBuffer geometry, mesh path (opaque, position-only out)
		const FShaderEntry*					VisBufferMeshShaderMasked = nullptr; // VisBuffer geometry, mesh path, masked (full interpolants)
		const FShaderEntry*					VisBufferVertexShader = nullptr;   // VisBuffer geometry, VS-emulation path
		const FShaderEntry*					MaskedVisBufferPixelShader  = nullptr;   // masked-only PS, VS path (flat VisID): opacity clip
		const FShaderEntry*					MaskedVisBufferPixelShaderPrim = nullptr; // masked-only PS, mesh path (SV_PrimitiveID)
		const FShaderEntry*					DeferredShader        = nullptr;   // deferred material pixel shader
		uint32                      		MaterialIndex = 0;                  // GPU material slot (deferred pixel classification)
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
