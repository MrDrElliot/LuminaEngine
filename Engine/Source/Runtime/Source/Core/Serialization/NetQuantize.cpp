#include "RuntimePCH.h"
#include "NetQuantize.h"
#include "NetArchive.h"
#include <cmath>

namespace Lumina::NetQuantize
{
    namespace
    {
        // --- bit-stream primitives (LSB-first via FNetArchive::SerializeBits) ---

        void PutBits(FNetArchive& Ar, uint64 Value, uint32 NumBits) { Ar.SerializeBits(&Value, NumBits); }

        uint64 GetBits(FNetArchive& Ar, uint32 NumBits)
        {
            uint64 Value = 0;
            Ar.SerializeBits(&Value, NumBits);
            return Value;
        }

        // LEB128 varint (7 data bits + continuation bit per byte).
        void WriteVarUInt64(FNetArchive& Ar, uint64 Value)
        {
            do
            {
                uint8 Byte = static_cast<uint8>(Value & 0x7F);
                Value >>= 7;
                if (Value != 0) { Byte |= 0x80; }
                Ar.SerializeBits(&Byte, 8);
            }
            while (Value != 0);
        }

        uint64 ReadVarUInt64(FNetArchive& Ar)
        {
            uint64 Result = 0;
            uint32 Shift  = 0;
            uint8  Byte   = 0;
            do
            {
                Byte = 0;
                Ar.SerializeBits(&Byte, 8);
                Result |= static_cast<uint64>(Byte & 0x7F) << Shift;
                Shift += 7;
            }
            while ((Byte & 0x80) != 0 && Shift < 64);
            return Result;
        }

        // Map signed <-> unsigned so small-magnitude values varint to few bytes.
        uint64 ZigZag(int64 V)   { return (static_cast<uint64>(V) << 1) ^ static_cast<uint64>(V >> 63); }
        int64  UnZigZag(uint64 V){ return static_cast<int64>(V >> 1) ^ -static_cast<int64>(V & 1); }

        int64 RoundToInt64(double D) { return static_cast<int64>(D < 0.0 ? D - 0.5 : D + 0.5); }

        // --- quaternion smallest-three constants ---
        constexpr uint32 QuatComponentBits = 15;
        constexpr uint32 QuatMaxQ          = (1u << QuatComponentBits) - 1;
        constexpr float  QuatRange         = 0.70710678118654752440f; // 1/sqrt(2): bound on the 3 smallest components
    }

    //~ FQuantizedVector

    FQuantizedVector FQuantizedVector::FromVector(const FVector3& V, double Quantum)
    {
        // Quantize in double so large world coords keep precision before the cast.
        FQuantizedVector Q;
        Q.X = RoundToInt64(static_cast<double>(V.x) / Quantum);
        Q.Y = RoundToInt64(static_cast<double>(V.y) / Quantum);
        Q.Z = RoundToInt64(static_cast<double>(V.z) / Quantum);
        return Q;
    }

    FVector3 FQuantizedVector::ToVector(double Quantum) const
    {
        return FVector3(
            static_cast<float>(static_cast<double>(X) * Quantum),
            static_cast<float>(static_cast<double>(Y) * Quantum),
            static_cast<float>(static_cast<double>(Z) * Quantum));
    }

    void FQuantizedVector::Write(FNetArchive& Ar) const
    {
        WriteVarUInt64(Ar, ZigZag(X));
        WriteVarUInt64(Ar, ZigZag(Y));
        WriteVarUInt64(Ar, ZigZag(Z));
    }

    void FQuantizedVector::Read(FNetArchive& Ar)
    {
        X = UnZigZag(ReadVarUInt64(Ar));
        Y = UnZigZag(ReadVarUInt64(Ar));
        Z = UnZigZag(ReadVarUInt64(Ar));
    }

    //~ FQuantizedQuat

    FQuantizedQuat FQuantizedQuat::FromQuat(const FQuat& Q)
    {
        const float C[4] = { Q.x, Q.y, Q.z, Q.w };

        // Find the largest-magnitude component; it's the one we drop and reconstruct.
        int   Largest = 0;
        float LargestAbs = std::fabs(C[0]);
        for (int i = 1; i < 4; ++i)
        {
            const float A = std::fabs(C[i]);
            if (A > LargestAbs) { LargestAbs = A; Largest = i; }
        }

        // q and -q are the same rotation; choose the sign that makes the dropped component positive,
        // so the reader can reconstruct it as +sqrt(...) with no sign bit.
        const float Sign = (C[Largest] < 0.0f) ? -1.0f : 1.0f;

        uint16 Out[3] = { 0, 0, 0 };
        int    o = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (i == Largest) { continue; }
            float A = C[i] * Sign;                       // in [-QuatRange, QuatRange]
            float Norm = (A / QuatRange + 1.0f) * 0.5f;  // -> [0, 1]
            Norm = Norm < 0.0f ? 0.0f : (Norm > 1.0f ? 1.0f : Norm);
            Out[o++] = static_cast<uint16>(Norm * QuatMaxQ + 0.5f);
        }

        FQuantizedQuat R;
        R.LargestIndex = static_cast<uint8>(Largest);
        R.A = Out[0];
        R.B = Out[1];
        R.C = Out[2];
        return R;
    }

    FQuat FQuantizedQuat::ToQuat() const
    {
        const uint16 In[3] = { A, B, C };
        float C4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float SumSq = 0.0f;
        int   o = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (i == static_cast<int>(LargestIndex)) { continue; }
            const float Norm = static_cast<float>(In[o++]) / static_cast<float>(QuatMaxQ);
            const float A4   = (Norm * 2.0f - 1.0f) * QuatRange;
            C4[i]  = A4;
            SumSq += A4 * A4;
        }
        C4[LargestIndex] = std::sqrt(SumSq < 1.0f ? 1.0f - SumSq : 0.0f);

        // Renormalize: quantization error can push the magnitude slightly off unit.
        const float LenSq = C4[0] * C4[0] + C4[1] * C4[1] + C4[2] * C4[2] + C4[3] * C4[3];
        const float Inv   = (LenSq > 1e-12f) ? 1.0f / std::sqrt(LenSq) : 1.0f;
        FQuat Q;
        Q.x = C4[0] * Inv;
        Q.y = C4[1] * Inv;
        Q.z = C4[2] * Inv;
        Q.w = C4[3] * Inv;
        return Q;
    }

    void FQuantizedQuat::Write(FNetArchive& Ar) const
    {
        PutBits(Ar, static_cast<uint64>(LargestIndex), 2);
        PutBits(Ar, static_cast<uint64>(A), QuatComponentBits);
        PutBits(Ar, static_cast<uint64>(B), QuatComponentBits);
        PutBits(Ar, static_cast<uint64>(C), QuatComponentBits);
    }

    void FQuantizedQuat::Read(FNetArchive& Ar)
    {
        LargestIndex = static_cast<uint8>(GetBits(Ar, 2));
        A = static_cast<uint16>(GetBits(Ar, QuatComponentBits));
        B = static_cast<uint16>(GetBits(Ar, QuatComponentBits));
        C = static_cast<uint16>(GetBits(Ar, QuatComponentBits));
    }

    //~ Free-function delegators (preserve the existing call sites and define the wire format once).

    void WritePackedVector(FNetArchive& Ar, const FVector3& V) { FQuantizedVector::FromVector(V).Write(Ar); }

    void ReadPackedVector(FNetArchive& Ar, FVector3& V)
    {
        FQuantizedVector Q;
        Q.Read(Ar);
        V = Q.ToVector();
    }

    void WritePackedQuat(FNetArchive& Ar, const FQuat& Q) { FQuantizedQuat::FromQuat(Q).Write(Ar); }

    void ReadPackedQuat(FNetArchive& Ar, FQuat& Q)
    {
        FQuantizedQuat R;
        R.Read(Ar);
        Q = R.ToQuat();
    }
}
