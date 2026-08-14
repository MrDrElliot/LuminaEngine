#pragma once

#include <concepts>
#include <EASTL/type_traits.h>
#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    struct RUNTIME_API FRandomStream
    {
        /** Default-constructs to a fixed, documented sequence - deterministic by default, by design. */
        constexpr FRandomStream() = default;

        explicit constexpr FRandomStream(uint64 InSeed, uint64 InSequence = 1u)
        {
            Seed(InSeed, InSequence);
        }

        /**
         * Reseeds the stream. InSequence selects one of 2^63 distinct streams, so two generators seeded
         * with the same value but different sequences do not correlate - use it to give subsystems (or
         * worker threads) independent sequences off one world seed.
         */
        constexpr void Seed(uint64 InSeed, uint64 InSequence = 1u)
        {
            State     = 0u;
            Increment = (InSequence << 1u) | 1u;
            (void)NextUInt32();
            State += InSeed;
            (void)NextUInt32();
        }

        /** Uniform over the full 32-bit range. */
        constexpr uint32 NextUInt32()
        {
            const uint64 Previous = State;
            State = Previous * 6364136223846793005ull + Increment;

            const uint32 XorShifted = uint32(((Previous >> 18u) ^ Previous) >> 27u);
            const uint32 Rotation   = uint32(Previous >> 59u);

            // Rotate right by Rotation. The mask keeps a zero rotation from shifting by 32 (UB).
            return (XorShifted >> Rotation) | (XorShifted << ((0u - Rotation) & 31u));
        }

        /** Uniform over the full 64-bit range. */
        constexpr uint64 NextUInt64()
        {
            const uint64 Low = NextUInt32();
            return (uint64(NextUInt32()) << 32u) | Low;
        }

        /**
         * Uniform over [0, Bound). Lemire's multiply-shift: one widening multiply, and the modulo in the
         * rejection branch runs with probability Bound/2^32, so in practice never.
         */
        constexpr uint32 NextUInt32Below(uint32 Bound)
        {
            if (Bound == 0u)
            {
                return 0u;
            }

            uint64 Product = uint64(NextUInt32()) * uint64(Bound);
            uint32 Low     = uint32(Product);
            if (Low < Bound)
            {
                const uint32 Threshold = (0u - Bound) % Bound;
                while (Low < Threshold)
                {
                    Product = uint64(NextUInt32()) * uint64(Bound);
                    Low     = uint32(Product);
                }
            }

            return uint32(Product >> 32u);
        }

        /** Uniform over [0, 1). 24 bits of mantissa, which is every float value that range can hold. */
        constexpr float NextFloat()
        {
            return float(NextUInt32() >> 8u) * (1.0f / 16777216.0f);
        }

        /** Uniform over [0, 1) with full double precision. */
        constexpr double NextDouble()
        {
            return double(NextUInt64() >> 11u) * (1.0 / 9007199254740992.0);
        }

        /** Uniform over [Min, Max]. Bounds are swapped if given the wrong way round. */
        template<std::integral T>
        constexpr T RandRange(T Min, T Max)
        {
            if (Min > Max)
            {
                const T Temp = Min;
                Min = Max;
                Max = Temp;
            }
            
            using UnsignedT = eastl::make_unsigned_t<T>;
            const uint64 Span = uint64(UnsignedT(UnsignedT(Max) - UnsignedT(Min)));

            if (Span >= 0xFFFFFFFFull)
            {
                const uint64 Draw = (Span == 0xFFFFFFFFFFFFFFFFull) ? NextUInt64() : (NextUInt64() % (Span + 1ull));
                return T(UnsignedT(uint64(UnsignedT(Min)) + Draw));
            }

            return T(UnsignedT(uint64(UnsignedT(Min)) + NextUInt32Below(uint32(Span) + 1u)));
        }

        /** Uniform over [Min, Max]. */
        constexpr float RandRange(float Min, float Max)
        {
            if (Min > Max)
            {
                const float Temp = Min;
                Min = Max;
                Max = Temp;
            }
            return Min + (Max - Min) * NextFloat();
        }

        constexpr bool RandBool()
        {
            // Top bit: the low bits of an LCG-derived word are the weakest, the high bits the strongest.
            return (NextUInt32() >> 31u) != 0u;
        }

        constexpr uint64 GetState() const     { return State; }
        constexpr uint64 GetIncrement() const { return Increment; }

    private:

        // Defaults are PCG32's documented initial state for sequence 1, so a default-constructed stream
        // is immediately usable and produces the same sequence everywhere.
        uint64 State     = 0x853c49e6748fea9bull;
        uint64 Increment = 0xda3e39cb94b95bdbull;
    };


    namespace Math
    {
        /**
         * The calling thread's stream. Seeded once per thread from system entropy, so sequences differ
         * per run and per thread. Lives in Runtime (not as an inline thread_local in this header) so
         * every module shares one stream per thread rather than one per module.
         */
        RUNTIME_API FRandomStream& ThreadRandomStream();

        /** Uniform over [Min, Max]. Non-deterministic; use an FRandomStream where the sequence matters. */
        template<std::integral T>
        [[nodiscard]] T RandRange(T Min, T Max)
        {
            return ThreadRandomStream().RandRange<T>(Min, Max);
        }

        /** Uniform over [Min, Max]. */
        [[nodiscard]] inline float RandRange(float Min, float Max)
        {
            return ThreadRandomStream().RandRange(Min, Max);
        }

        /** Uniform over [0, 1). */
        [[nodiscard]] inline float Rand01()
        {
            return ThreadRandomStream().NextFloat();
        }

        [[nodiscard]] inline bool RandBool()
        {
            return ThreadRandomStream().RandBool();
        }
    }
}
