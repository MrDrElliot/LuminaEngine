#include "Includes/SceneGlobals.slang"

// Stamps the proxy's VisBuffer word, which ProxyVisBufferVert.slang built with the proxy bit set.

[shader("fragment")]
uint FragmentMain(nointerpolation uint VisID : VISID) : SV_Target0
{
    return VisID;
}
