#pragma once

#include "Containers/SegmentArray.h"

namespace Lumina
{
    struct FShaderEntry;

    // Weak, generational reference to a shader-library entry. Split out from ShaderLibrary.h so the many
    // headers that only need to STORE one (material assets, draw commands, resolve cache, pipeline keys)
    // do not pull in the library, the RHI and the compiler with it.
    //
    // Weak is the point: the library entry is content-keyed and shared between every material that compiles
    // to identical bytecode, so only the owning CMaterial stages hold strong references. Everything else
    // holds one of these and learns that its cached shaders were superseded because
    // FShaderLibrary::Resolve returns null -- never by dereferencing freed memory.
    using FShaderH = THandle<FShaderEntry>;
}
