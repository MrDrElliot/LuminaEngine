#pragma once

#include "Containers/Array.h"
#include "Containers/String.h"
#include "Core/Profiler/Profile.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"
#include <string.h>


namespace Lumina::Hash
{
	// XXHash: default hashing algorithm for the engine.

	/** Golden-ratio odd constant, 64-bit. Steps the seed so a run of equal values still separates. */
	constexpr uint64 GGoldenRatio64 = 0x9e3779b97f4a7c15ull;

	/**
	 * splitmix64's finalizer: two multiply-xorshift rounds that avalanche every input bit across all 64.
	 */
	FORCEINLINE constexpr uint64 Mix64(uint64 Value) noexcept
	{
		Value ^= Value >> 30;
		Value *= 0xbf58476d1ce4e5b9ull;
		Value ^= Value >> 27;
		Value *= 0x94d049bb133111ebull;
		Value ^= Value >> 31;
		return Value;
	}

	/**
	 * Mixes Value into Seed.
	 */
	FORCEINLINE void HashCombine(size_t& Seed, size_t Value) noexcept
	{
		Seed = (size_t)Mix64((uint64)Seed + GGoldenRatio64 + Mix64((uint64)Value));
	}

	namespace XXHash
	{
		RUNTIME_API uint32 GetHash32(const void* Data, size_t Size, uint32 Seed = 0);

		inline uint32 GetHash32(const FString& string, uint32 Seed = 0)
		{
			return GetHash32(string.c_str(), string.length(), Seed);
		}
		
		inline uint32 GetHash32(const char* String)
		{
			return GetHash32(String, strlen(String));
		}

		inline uint32 GetHash32(float Value, uint32 Seed = 0)
		{
			return GetHash32(&Value, sizeof(float), Seed);
		}

		inline uint32 GetHash32(const Blob& data, uint32 Seed = 0)
		{
			return GetHash32(data.data(), data.size(), Seed);
		}

		RUNTIME_API uint64 GetHash64(void const* Data, size_t Size, uint64 Seed = 0);

		inline uint64 GetHash64(const FString& String, uint64 Seed = 0)
		{
			return GetHash64(String.c_str(), String.length(), Seed);
		}

		// Unseeded for the same reason as GetHash32(const char*) above.
		inline uint64 GetHash64(const char* String)
		{
			return GetHash64(String, strlen(String));
		}

		inline uint64 GetHash64(const Blob& data, uint64 Seed = 0)
		{
			return GetHash64(data.data(), data.size(), Seed);
		}
	}

	// FNV1a: constexpr hash. Use only for code-only features (custom RTTI, etc.).
	namespace FNV1a
	{
		constexpr uint32 GOffsetBasis32	= 0x811c9dc5;
		constexpr uint32 GPrime32		= 0x1000193;
		constexpr uint64 GOffsetBasis64	= 0xcbf29ce484222325;
		constexpr uint64 GPrime64		= 0x100000001b3;
		
		constexpr static uint32 GetHash32(const char* const str, const uint32 val = GOffsetBasis32)
		{
			return (str[0] == '\0') ? val : GetHash32(&str[1], (val ^ static_cast<uint8>(str[0])) * GPrime32);
		}

		constexpr static uint64 GetHash64(char const* const str, const uint64 val = GOffsetBasis64)
		{
			return (str[0] == '\0') ? val : GetHash64(&str[1], (val ^ static_cast<uint64>(static_cast<uint8>(str[0]))) * GPrime64);
		}
		
		constexpr static uint16 GetHash16(const char* const str)
		{
			return static_cast<uint16>((GetHash32(str) >> 16) ^ (GetHash32(str) & 0xFFFFu));
		}
	}

	// Default Lumina hashing functions. Seeds mirror the XXHash layer, and for the same reason the
	// const char* forms below take none: a seed there makes every GetHash32(Ptr, Length) ambiguous.
	FORCEINLINE uint32 GetHash32(const FString& string, uint32 Seed = 0)
	{
		return XXHash::GetHash32(string.c_str(), string.length(), Seed);
	}

	template<size_t S>
	FORCEINLINE uint32 GetHash32(const TFixedString<S>& string, uint32 Seed = 0)
	{
		return XXHash::GetHash32(string.c_str(), string.length(), Seed);
	}

	FORCEINLINE uint32 GetHash32(const char* String)
	{
		return XXHash::GetHash32(String, strlen(String));
	}

	FORCEINLINE uint32 GetHash32(const void* Data, size_t size, uint32 Seed = 0)
	{
		return XXHash::GetHash32(Data, size, Seed);
	}

	FORCEINLINE uint32 GetHash32(const Blob& data, uint32 Seed = 0)
	{
		return XXHash::GetHash32(data.data(), data.size(), Seed);
	}

	FORCEINLINE uint64 GetHash64(const FString& string, uint64 Seed = 0)
	{
		return XXHash::GetHash64(string.c_str(), string.length(), Seed);
	}

	template<size_t S>
	FORCEINLINE uint64 GetHash64(const TFixedString<S>& string, uint64 Seed = 0)
	{
		return XXHash::GetHash64(string.c_str(), string.length(), Seed);
	}

	FORCEINLINE uint64 GetHash64(const char* String)
	{
		return XXHash::GetHash64(String, strlen(String));
	}

	FORCEINLINE uint64 GetHash64(const void* Data, size_t size, uint64 Seed = 0)
	{
		return XXHash::GetHash64(Data, size, Seed);
	}

	FORCEINLINE uint64 GetHash64(const Blob& Data, uint64 Seed = 0)
	{
		return XXHash::GetHash64(Data.data(), Data.size(), Seed);
	}

	template<ContiguousContainer T>
	FORCEINLINE uint64 GetHash64(const T& Array)
	{
		static_assert(eastl::is_trivially_copyable_v<typename T::value_type>,
			"GetHash64(container) hashes raw element bytes; the element type must be trivially copyable. "
			"For anything else, loop and HashCombine each element.");

		return XXHash::GetHash64(Array.data(), Array.size() * sizeof(typename T::value_type));
	}

	template<typename T>
	concept HasHasher = requires(const T & Value)
	{
		{ GetTypeHash(Value) } -> std::convertible_to<size_t>;
	};

	template<typename T>
	concept HashEASTLHasher = requires(const T & Value)
	{
		{ eastl::hash<T>()(Value) } -> std::convertible_to<size_t>;
	};

	/**
	 * Strictly ordered fallbacks: GetTypeHash, then eastl::hash, then an enum's underlying type.
	 */
	template<typename T>
	requires HasHasher<T>
	size_t GetHash(const T& Value) noexcept
	{
		return GetTypeHash(Value);
	}

	template <typename T>
	requires (!HasHasher<T> && HashEASTLHasher<T>)
	size_t GetHash(const T& value) noexcept
	{
		return eastl::hash<T>()(value);
	}

	template <typename T>
	requires eastl::is_enum_v<T> && (!HasHasher<T>) && (!HashEASTLHasher<T>)
	size_t GetHash(const T& value) noexcept
	{
		using UnderlyingType = eastl::underlying_type_t<T>;
		return eastl::hash<UnderlyingType>()(static_cast<UnderlyingType>(value));
	}

	// All three overloads funnel through the one HashCombine(size_t&, size_t) above, so they share its
	// mixing and cannot drift from each other.
	template <class T>
	void HashCombine(size_t& Seed, const T& V) noexcept
	{
		HashCombine(Seed, GetHash(V));
	}
	
	/**
	 * Floats hash by bit pattern.
	 * -0.0 is folded onto +0.0 because the two compare equal, leaving them apart gives a container two
	 * different buckets for one key, and a lookup that == says must hit would miss.
	 */
	template <std::floating_point T>
	void HashCombine(size_t& Seed, const T& V) noexcept
	{
		const T Value = (V == T(0)) ? T(0) : V;
		HashCombine(Seed, (size_t)XXHash::GetHash64(&Value, sizeof(Value)));
	}

}

template<typename T>
requires (Lumina::Hash::HasHasher<T>)
struct eastl::hash<T>
{
	size_t operator()(const T& Value) const
	{
		return GetTypeHash(Value);
	}
};