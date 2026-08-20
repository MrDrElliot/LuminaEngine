#pragma once
#include <bit>

#include "Platform/GenericPlatform.h"

// Min() is the smallest POSITIVE finite value for floats, as in std; the most negative is always Lowest().
template <typename T>
struct TNumericLimits;

template <>
struct TNumericLimits<uint8>
{
    static constexpr uint8 Min()    { return 0; }
    static constexpr uint8 Lowest() { return 0; }
    static constexpr uint8 Max()    { return 0xFFu; }
};

template <>
struct TNumericLimits<uint16>
{
    static constexpr uint16 Min()    { return 0; }
    static constexpr uint16 Lowest() { return 0; }
    static constexpr uint16 Max()    { return 0xFFFFu; }
};

template <>
struct TNumericLimits<uint32>
{
    static constexpr uint32 Min()    { return 0; }
    static constexpr uint32 Lowest() { return 0; }
    static constexpr uint32 Max()    { return 0xFFFFFFFFu; }
};

template <>
struct TNumericLimits<uint64>
{
    static constexpr uint64 Min()    { return 0; }
    static constexpr uint64 Lowest() { return 0; }
    static constexpr uint64 Max()    { return 0xFFFFFFFFFFFFFFFFull; }
};

template <>
struct TNumericLimits<int8>
{
    static constexpr int8 Min()    { return -128; }
    static constexpr int8 Lowest() { return -128; }
    static constexpr int8 Max()    { return 127; }
};

template <>
struct TNumericLimits<int16>
{
    static constexpr int16 Min()    { return -32768; }
    static constexpr int16 Lowest() { return -32768; }
    static constexpr int16 Max()    { return 32767; }
};

template <>
struct TNumericLimits<int32>
{
    // A subtraction because the literal 2147483648 does not fit an int32.
    static constexpr int32 Min()    { return -2147483647 - 1; }
    static constexpr int32 Lowest() { return -2147483647 - 1; }
    static constexpr int32 Max()    { return 2147483647; }
};

template <>
struct TNumericLimits<int64>
{
    static constexpr int64 Min()    { return -9223372036854775807ll - 1; }
    static constexpr int64 Lowest() { return -9223372036854775807ll - 1; }
    static constexpr int64 Max()    { return 9223372036854775807ll; }
};

template <>
struct TNumericLimits<float>
{
    static constexpr float Min()      { return 1.175494351e-38f; }
    static constexpr float Lowest()   { return -3.402823466e+38f; }
    static constexpr float Max()      { return 3.402823466e+38f; }
    static constexpr float Epsilon()  { return 1.192092896e-07f; }

    // Bit patterns rather than a builtin, so both stay constant expressions on every toolchain.
    static constexpr float Infinity() { return std::bit_cast<float>(0x7F800000u); }
    static constexpr float QuietNaN() { return std::bit_cast<float>(0x7FC00000u); }
};

template <>
struct TNumericLimits<double>
{
    static constexpr double Min()      { return 2.2250738585072014e-308; }
    static constexpr double Lowest()   { return -1.7976931348623158e+308; }
    static constexpr double Max()      { return 1.7976931348623158e+308; }
    static constexpr double Epsilon()  { return 2.2204460492503131e-016; }

    static constexpr double Infinity() { return std::bit_cast<double>(0x7FF0000000000000ull); }
    static constexpr double QuietNaN() { return std::bit_cast<double>(0x7FF8000000000000ull); }
};

template <typename T> struct TNumericLimits<const          T> : TNumericLimits<T> {};
template <typename T> struct TNumericLimits<      volatile T> : TNumericLimits<T> {};
template <typename T> struct TNumericLimits<const volatile T> : TNumericLimits<T> {};
