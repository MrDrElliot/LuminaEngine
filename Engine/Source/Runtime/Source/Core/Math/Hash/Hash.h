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

	inline void HashCombine(size_t& seed, size_t value)
	{
		seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
	}

	namespace XXHash
	{
		RUNTIME_API uint32 GetHash32(const void* Data, size_t size);

		inline uint32 GetHash32(const FString& string)
		{
			return GetHash32(string.c_str(), string.length());
		}

		inline uint32 GetHash32(const char* String)
		{
			return GetHash32(String, strlen(String));
		}

		inline uint32 GetHash32(float Value)
		{
			return GetHash32(&Value, sizeof(float));
		}

		inline uint32 GetHash32(const Blob& data)
		{
			return GetHash32(data.data(), data.size());
		}

		RUNTIME_API uint64 GetHash64(void const* Data, size_t Size);

		inline uint64 GetHash64(const FString& String)
		{
			return GetHash64(String.c_str(), String.length());
		}

		inline uint64 GetHash64(const char* String)
		{
			return GetHash64(String, strlen(String));
		}

		inline uint64 GetHash64(const Blob& data)
		{
			return GetHash64(data.data(), data.size());
		}
	}

	// FNV1a: constexpr hash. Use only for code-only features (custom RTTI, etc.).
	namespace FNV1a
	{
		constexpr uint16 GConstValue16          = 0x811C;
		constexpr uint16 GDefaultOffsetBasis16  = 0x0193;
		constexpr uint32 GConstValue32			= 0x811c9dc5;
		constexpr uint32 GDefaultOffsetBasis32	= 0x1000193;
		constexpr uint64 GConstValue64			= 0xcbf29ce484222325;
		constexpr uint64 GDefaultOffsetBasis64	= 0x100000001b3;

		constexpr static uint16 GetHash16(const char* const str, const uint16 val = GConstValue16)
		{
			return (str[0] == '\0') ? val : GetHash16(&str[1], static_cast<uint16>((val ^ static_cast<uint8>(str[0])) * GDefaultOffsetBasis16));
		}
		
		constexpr static uint32 GetHash32(const char* const str, const uint32 val = GConstValue32)
		{
			return (str[0] == '\0') ? val : GetHash32(&str[1], ((uint64)val ^ static_cast<uint32>(str[0])) * GDefaultOffsetBasis32);
		}

		constexpr static uint64 GetHash64(char const* const str, const uint64 val = GConstValue64)
		{
			return (str[0] == '\0') ? val : GetHash64(&str[1], ((uint64)val ^ static_cast<uint64>(str[0])) * GDefaultOffsetBasis64);
		}
	}

	// Default Lumina hashing functions.
	FORCEINLINE uint32 GetHash32(const FString& string)
	{
		return XXHash::GetHash32(string.c_str(), string.length());
	}

	template<size_t S>
	FORCEINLINE uint32 GetHash32(const TFixedString<S>& string)
	{
		return XXHash::GetHash32(string.c_str(), string.length());
	}

	FORCEINLINE uint32 GetHash32(const char* String)
	{
		return XXHash::GetHash32(String, strlen(String));
	}

	FORCEINLINE uint32 GetHash32(const void* Data, size_t size)
	{
		return XXHash::GetHash32(Data, size);
	}

	FORCEINLINE uint32 GetHash32(const Blob& data)
	{
		return XXHash::GetHash32(data.data(), data.size());
	}

	FORCEINLINE uint64 GetHash64(const FString& string)
	{
		return XXHash::GetHash64(string.c_str(), string.length());
	}

	template<size_t S>
	FORCEINLINE uint64 GetHash64(const TFixedString<S>& string)
	{
		return XXHash::GetHash64(string.c_str(), string.length());
	}

	FORCEINLINE uint64 GetHash64(const char* String)
	{
		return XXHash::GetHash64(String, strlen(String));
	}

	FORCEINLINE uint64 GetHash64(const void* Data, size_t size)
	{
		return XXHash::GetHash64(Data, size);
	}

	FORCEINLINE uint64 GetHash64(const Blob& Data)
	{
		return XXHash::GetHash64(Data.data(), Data.size());
	}

	template<ContiguousContainer T>
	FORCEINLINE uint64 GetHash64(const T& Array)
	{
		return XXHash::GetHash64(Array.data(), Array.size() * sizeof(T::value_type));
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

	template <typename T>
	requires eastl::is_enum_v<T> && (!HasHasher<T>) && (!HashEASTLHasher<T>)
	size_t GetHash(const T& value) noexcept
	{
		using UnderlyingType = eastl::underlying_type_t<T>;
		return eastl::hash<UnderlyingType>()(static_cast<UnderlyingType>(value));
	}

	template<typename T>
	requires (HasHasher<T> && !HashEASTLHasher<T>)
	size_t GetHash(const T& Value) noexcept
	{
		return GetTypeHash(Value);
	}

	template <typename T>
		requires (HashEASTLHasher<T> && !HasHasher<T>)
	size_t GetHash(const T& value) noexcept
	{
		return eastl::hash<T>()(value);
	}

	template <class T>
	void HashCombine(size_t& seed, const T& V) noexcept
	{
		seed ^= GetHash(V) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
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