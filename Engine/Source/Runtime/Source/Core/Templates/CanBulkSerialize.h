#pragma once


namespace Lumina
{
	template <typename T>
	struct TCanBulkSerialize : std::false_type { };
	
	template<typename T>
	requires(std::is_trivial_v<T>)
	struct TCanBulkSerialize<T> : std::true_type { };
	
}
