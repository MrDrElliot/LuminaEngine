// Terrain depth pre-pass pixel shader: stamps 'empty' (0) into the R32_UINT VisBuffer wherever terrain
// is the closest opaque surface, so classify/deferred skip mesh pixels the terrain covers instead of
// shading them and having the terrain pass overdraw the result.

[shader("fragment")]
uint FragmentMain() : SV_Target0
{
    return 0u;
}
