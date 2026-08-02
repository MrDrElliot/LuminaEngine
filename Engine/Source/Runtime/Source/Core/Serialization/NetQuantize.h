#pragma once

#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FNetArchive;

    // Compact wire encodings for replicated transforms, written through FNetArchive's bit stream.
    namespace NetQuantize
    {
        inline constexpr double PositionQuantum = 0.001;  // 1 mm
        inline constexpr double ScaleQuantum    = 0.0001; // scale precision

        // Quantized position/scale: per-axis int64 in quantum units. int64 (not int32) matches the wire
        // encoder and avoids overflow past ~2.1 km at 1 mm. Exact integer compare drives change detection.
        struct FQuantizedVector
        {
            int64 X = 0;
            int64 Y = 0;
            int64 Z = 0;

            static FQuantizedVector FromVector(const FVector3& V, double Quantum = PositionQuantum);
            FVector3 ToVector(double Quantum = PositionQuantum) const;

            bool operator==(const FQuantizedVector& O) const { return X == O.X && Y == O.Y && Z == O.Z; }
            bool operator!=(const FQuantizedVector& O) const { return !(*this == O); }

            void Write(FNetArchive& Ar) const; // ZigZag + VarUInt64 per axis
            void Read (FNetArchive& Ar);
        };

        // Quantized rotation: smallest-three. Drop the largest-magnitude component, send its index (2 bits)
        // plus the other three as 15-bit normalized values; reconstruct the dropped one as +sqrt(1-sum).
        struct FQuantizedQuat
        {
            uint8  LargestIndex = 3;
            uint16 A = 0;
            uint16 B = 0;
            uint16 C = 0;

            static FQuantizedQuat FromQuat(const FQuat& Q);
            FQuat ToQuat() const; // reconstruct + renormalize

            bool operator==(const FQuantizedQuat& O) const { return LargestIndex == O.LargestIndex && A == O.A && B == O.B && C == O.C; }
            bool operator!=(const FQuantizedQuat& O) const { return !(*this == O); }

            void Write(FNetArchive& Ar) const;
            void Read (FNetArchive& Ar);
        };

        // Free-function form; delegates to the value types above (single source of truth for the format).
        void WritePackedVector(FNetArchive& Ar, const FVector3& V);
        void ReadPackedVector (FNetArchive& Ar, FVector3& V);

        void WritePackedQuat(FNetArchive& Ar, const FQuat& Q);
        void ReadPackedQuat (FNetArchive& Ar, FQuat& Q);
    }
}
