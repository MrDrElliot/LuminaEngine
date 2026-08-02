#pragma once

namespace Lumina::Memory
{
	FORCEINLINE void Memcpy(void* RESTRICT Destination, void* RESTRICT Source, size_t SrcSize)
	{
		std::memcpy(Destination, Source, SrcSize);
	}

	FORCEINLINE void Memcpy(void* RESTRICT Destination, const void* RESTRICT Source, size_t SrcSize)
	{
		std::memcpy(Destination, Source, SrcSize);
	}
}