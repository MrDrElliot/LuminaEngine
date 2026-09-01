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


// Standard library: language support and the few C headers everything reaches for. The containers,
// algorithms and function objects are ours now, so their std counterparts are deliberately absent.
#include <utility>
#include <type_traits>
#include <initializer_list>
#include <new>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <atomic>

#include <xxhash.h>

// The ECS facade is reached by most world and component headers, so it is parsed once here.

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
#include "Containers/Algorithm.h"
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

// Tracy costs 165ms in any translation unit that reaches it, and 26 of 28 unity files do.
#include "Core/Profiler/Profile.h"
