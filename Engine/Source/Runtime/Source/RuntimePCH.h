#pragma once

// Win32 hygiene
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NODRAWTEXT
#define NODRAWTEXT
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NORESOURCE
#define NORESOURCE
#endif


// Standard library: only headers that are small, ubiquitous, or pulled transitively by EASTL.
#include <memory>
#include <string>
#include <utility>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <tuple>
#include <vector>
#include <array>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <functional>
#include <cmath>
#include <numeric>
#include <chrono>
#include <atomic>
#include <type_traits>
#include <optional>
#include <limits>
#include <cassert>

#include <EASTL/type_traits.h>
#include <EASTL/utility.h>
#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <EASTL/map.h>
#include <EASTL/unordered_map.h>
#include <EASTL/set.h>
#include <EASTL/unordered_set.h>
#include <EASTL/hash_set.h>
#include <EASTL/hash_map.h>
#include <EASTL/fixed_hash_set.h>
#include <EASTL/fixed_hash_map.h>
#include <EASTL/fixed_vector.h>
#include <EASTL/fixed_string.h>
// Expensive, but Core/Variant pulls it into ~half of all TUs; amortize the parse here.
#include <EASTL/variant.h>
#include <EASTL/atomic.h>
#include <EASTL/sort.h>
#include <EASTL/memory.h>
#include <EASTL/string.h>
#include <EASTL/string_view.h>
#include <EASTL/algorithm.h>
#include <EASTL/functional.h>
#include <EASTL/unique_ptr.h>
#include <EASTL/shared_ptr.h>
#include <EASTL/weak_ptr.h>

// entt is reached directly by 48+ files; keeping it here parses it once instead of per-TU.
#include <entt/entt.hpp>
#include <xxhash.h>

// Math types used nearly everywhere; headers also carry REFLECTION_PARSER stubs the reflector walks.
#include "Core/Math/Vector/Vector.h"
#include "Core/Math/Quat/Quat.h"
#include "Core/Math/Matrix/Matrix.h"
#include "Core/Math/Matrix/MatrixMath.h"
#include "Core/Math/Scalar.h"
#include "Core/Math/Packing.h"
#include "Core/Math/MathString.h"

// ModuleAPI carries LUMINA_SCRIPT_API, and the build force-includes it AFTER this file, so any
// header below that declares an interop thunk would see it undefined without this. RUNTIME_API and
// the other per-module export macros do not come from here; the build system defines them on the
// command line, so they are in scope before the first line of any translation unit.
#include "ModuleAPI.h"

// Core headers already reached by 87-95% of TUs. Parsing them here once instead of ~400 times is
// nearly free in cascade terms: editing one already dirties almost every TU, so routing it through
// the PCH trades "rebuild ~400 TUs" for "rebuild 456", and buys back the per-TU parse.
// Anything below 85% fan-in does NOT belong here - it would start dirtying TUs that don't use it.
#include "Memory/Memory.h"
#include "Core/Assertions/Assert.h"
#include "Containers/String.h"
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
