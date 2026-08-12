#include "RuntimePCH.h"
#include "MaterialInterface.h"

// Intentionally near-empty. This held the material -> texture publishing that fed the old CPU streaming
// estimate; texture streaming is driven by GPU feedback keyed on the bindless slot now, so a material no
// longer has to tell anyone which textures it samples. Kept as a TU so the header keeps a home for any
// future out-of-line CMaterialInterface method.
