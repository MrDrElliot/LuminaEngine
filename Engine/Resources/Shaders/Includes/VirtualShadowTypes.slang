#pragma once

// One level of the sun's virtual shadow, mirrored by FVirtualShadowLevelGPU in SceneRenderTypes.h.
struct FVirtualShadowLevel
{
    float4x4 ViewProjection;
    // The global page under the level's top left virtual page, so a page's identity survives the level scrolling.
    int2     PageOrigin;
    float    TexelWorld;
    float    DepthRange;
    uint     bInvalidate;
    uint     bActive;
    uint     _Pad0;
    uint     _Pad1;
};

// One virtual page. Pages sit at their global page modulo the level's width, so a scrolling level keeps the pages it still covers.
struct FVirtualShadowPage
{
    uint Entry;
    // The global page the physical page was drawn for, in units of a level's width, so a page that wrapped around reads as new.
    uint Tag;
    uint LastRequested;
    uint State;
};
