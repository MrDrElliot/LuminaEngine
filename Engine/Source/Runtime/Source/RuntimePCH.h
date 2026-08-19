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


// Standard library: only headers that are small and ubiquitous.
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

#include "ModuleAPI.h"

#include "Memory/Memory.h"
#include "Core/Assertions/Assert.h"
#include "Containers/Vector.h"
#include "Containers/VectorOps.h"
#include "Containers/HashTable.h"
#include "Containers/Pair.h"
#include "Containers/Span.h"
#include "Containers/StaticArray.h"
#include "Containers/String.h"
#include "Containers/Name.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
