#include "Includes/MeshletGeometry.slang"
#include "Includes/VirtualShadow.slang"

// An opaque caster's fragment, kept as the nearest depth in its physical page since nothing here has a depth target.

[shader("fragment")]
void FragmentMain(float4 Position : SV_Position, uint ViewportIndex : SV_ViewportArrayIndex)
{
    WriteVirtualShadowDepth(MeshletPass().CullViewIndex + (IsMultiViewShadowDraw() ? ViewportIndex : 0u), Position);
}
