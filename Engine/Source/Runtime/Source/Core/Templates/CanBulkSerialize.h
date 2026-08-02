#pragma once

#include <EASTL/type_traits.h>

namespace Lumina
{
	template <typename T>
	struct TCanBulkSerialize : eastl::false_type { };
	
	template<typename T>
	requires(eastl::is_trivial_v<T>)
	struct TCanBulkSerialize<T> : eastl::true_type { };
	
}
