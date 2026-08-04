#pragma once

//  Writable half of the global texture heap (set 0, binding 2).

#include "GlobalRHI.slang"

// ---------------------------------------------------------------------------
//  Global storage-image heap.
// ---------------------------------------------------------------------------
//
// Deliberately NOT in GlobalRHI.slang. One binding carries every storage view the
// engine registers, and those views are not all the same type: a plain 2D target is
// a VK_IMAGE_VIEW_TYPE_2D view, a cube RW target is a 2D-array one. Each alias below
// claims the whole heap is its own type, so validation checks every live slot against
// whichever alias the module declares -- and a module that declares the array alias
// while some slot legitimately holds a 2D view reports VUID-vkCmdDraw-viewType-07752
// against code that never touched the heap at all.
//
// Keeping the declarations out of the always-included header means a shader is only
// type-checked against the aliases it asked for. Every user is a compute shader; if a
// graphics shader ever needs one of these, including this header is the deliberate act
// that puts it back in scope.

[[vk::binding(2, 0)]] RWTexture2D<float4>                       gRWTextures2D[];
[[vk::binding(2, 0)]] RWTexture3D<float4>                       gRWTextures3D[];
// Cube storage views are illegal in Vulkan; cube RW slots are registered as
// 2D-array views (HeapWriteRWTexture) and written via [int3(x, y, face)].
[[vk::binding(2, 0)]] RWTexture2DArray<float4>                  gRWTextures2DArray[];
// Coherent alias for cross-workgroup hand-offs (SPD-style single-pass chains).
[[vk::binding(2, 0)]] globallycoherent RWTexture2D<float4>      gRWTextures2DCoherent[];
