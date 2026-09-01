#pragma once

#include "VoxelTypes.h"

namespace Grain::Noise
{
    inline float Hash11(uint32 N)
    {
        N = (N ^ 61u) ^ (N >> 16);
        N *= 9u;
        N = N ^ (N >> 4);
        N *= 0x27d4eb2du;
        N = N ^ (N >> 15);
        return float(N & 0x00FFFFFFu) / float(0x01000000u);
    }

    inline float Hash21(int32 X, int32 Y)
    {
        return Hash11(uint32(X) * 73856093u ^ uint32(Y) * 19349663u);
    }

    inline float Hash31(int32 X, int32 Y, int32 Z)
    {
        return Hash11(uint32(X) * 73856093u ^ uint32(Y) * 19349663u ^ uint32(Z) * 83492791u);
    }

    inline float Smooth(float T)
    {
        return T * T * (3.0f - 2.0f * T);
    }

    inline float Value2D(float X, float Y)
    {
        const float FloorX = Math::Floor(X);
        const float FloorY = Math::Floor(Y);
        const int32 IX = int32(FloorX);
        const int32 IY = int32(FloorY);
        const float FX = Smooth(X - FloorX);
        const float FY = Smooth(Y - FloorY);

        const float A = Hash21(IX, IY);
        const float B = Hash21(IX + 1, IY);
        const float C = Hash21(IX, IY + 1);
        const float D = Hash21(IX + 1, IY + 1);

        return Math::Lerp(Math::Lerp(A, B, FX), Math::Lerp(C, D, FX), FY);
    }

    inline float Value3D(float X, float Y, float Z)
    {
        const float FloorX = Math::Floor(X);
        const float FloorY = Math::Floor(Y);
        const float FloorZ = Math::Floor(Z);
        const int32 IX = int32(FloorX);
        const int32 IY = int32(FloorY);
        const int32 IZ = int32(FloorZ);
        const float FX = Smooth(X - FloorX);
        const float FY = Smooth(Y - FloorY);
        const float FZ = Smooth(Z - FloorZ);

        const float A = Math::Lerp(Math::Lerp(Hash31(IX, IY, IZ), Hash31(IX + 1, IY, IZ), FX),
                                   Math::Lerp(Hash31(IX, IY + 1, IZ), Hash31(IX + 1, IY + 1, IZ), FX), FY);
        const float B = Math::Lerp(Math::Lerp(Hash31(IX, IY, IZ + 1), Hash31(IX + 1, IY, IZ + 1), FX),
                                   Math::Lerp(Hash31(IX, IY + 1, IZ + 1), Hash31(IX + 1, IY + 1, IZ + 1), FX), FY);
        return Math::Lerp(A, B, FZ);
    }

    inline float Fbm2D(float X, float Y, int32 Octaves)
    {
        float Sum = 0.0f;
        float Amplitude = 0.5f;
        float Total = 0.0f;

        for (int32 i = 0; i < Octaves; ++i)
        {
            Sum += Value2D(X, Y) * Amplitude;
            Total += Amplitude;
            X = X * 2.03f + 17.3f;
            Y = Y * 2.03f - 9.1f;
            Amplitude *= 0.5f;
        }
        return Sum / Math::Max(Total, 0.0001f);
    }

    inline float Fbm3D(float X, float Y, float Z, int32 Octaves)
    {
        float Sum = 0.0f;
        float Amplitude = 0.5f;
        float Total = 0.0f;

        for (int32 i = 0; i < Octaves; ++i)
        {
            Sum += Value3D(X, Y, Z) * Amplitude;
            Total += Amplitude;
            X = X * 2.07f + 11.7f;
            Y = Y * 2.07f + 5.9f;
            Z = Z * 2.07f - 23.1f;
            Amplitude *= 0.5f;
        }
        return Sum / Math::Max(Total, 0.0001f);
    }

    // Folded so the peaks form continuous crests rather than isolated blobs.
    inline float Ridged2D(float X, float Y, int32 Octaves)
    {
        float Sum = 0.0f;
        float Amplitude = 0.5f;
        float Total = 0.0f;

        for (int32 i = 0; i < Octaves; ++i)
        {
            const float Folded = 1.0f - Math::Abs(Value2D(X, Y) * 2.0f - 1.0f);
            Sum += Folded * Folded * Amplitude;
            Total += Amplitude;
            X = X * 2.11f + 31.7f;
            Y = Y * 2.11f - 13.3f;
            Amplitude *= 0.5f;
        }
        return Sum / Math::Max(Total, 0.0001f);
    }
}
