#pragma once

#include "Containers/Format.h"
#include "Core/Math/Matrix/MatrixMath.h"
#include "Core/Math/SIMD/VQuat1.h"
#include "Core/Math/TransformFwd.h"
#include "Core/Reflection/ReflectionMacros.h"
#include "Transform.generated.h"

// SIMD-backed transform.

namespace Lumina
{
    class FArchive;

    // The reflected shape is 40 bytes and the real one 48, so Transform.cs mirrors the padding by hand.
    REFLECT(ReflectedName = "FTransform", NoCSharp, CSharpValueMirror, MinimalAPI)
    struct alignas(16) VTransform
    {
        GENERATED_BODY()

        PROPERTY(Editable, ReflectAs = "FVector3")
        SIMD::VFloat4 Location;   // x, y, z, 0

        PROPERTY(Editable, ReflectAs = "FQuat")
        SIMD::VFloat4 Rotation;   // x, y, z, w

        PROPERTY(Editable, ReflectAs = "FVector3")
        SIMD::VFloat4 Scale;      // x, y, z, 1  (pad lane 1 so Inverse's 1/Scale never divides by 0)

        RUNTIME_API VTransform()
            : Location(0.0f, 0.0f, 0.0f, 0.0f)
            , Rotation(0.0f, 0.0f, 0.0f, 1.0f)
            , Scale(1.0f, 1.0f, 1.0f, 1.0f)
        {}

        RUNTIME_API explicit VTransform(const FVector3& InLocation)
            : Location(InLocation.x, InLocation.y, InLocation.z, 0.0f)
            , Rotation(0.0f, 0.0f, 0.0f, 1.0f)
            , Scale(1.0f, 1.0f, 1.0f, 1.0f)
        {}

        RUNTIME_API VTransform(const FVector3& InLocation, const FVector3& EulerAngles, const FVector3& InScale)
            : Location(InLocation.x, InLocation.y, InLocation.z, 0.0f)
            , Rotation(SIMD::LoadQuat(FQuat(Math::Radians(EulerAngles))))
            , Scale(InScale.x, InScale.y, InScale.z, 1.0f)
        {}

        RUNTIME_API explicit VTransform(const FMatrix4& InMatrix)
        {
            FVector3 S, L, Skew;
            FQuat R;
            FVector4 Perspective;
            Math::Decompose(InMatrix, S, R, L, Skew, Perspective);
            Location = SIMD::VFloat4(L.x, L.y, L.z, 0.0f);
            Rotation = SIMD::LoadQuat(R);
            Scale    = SIMD::VFloat4(S.x, S.y, S.z, 1.0f);
        }
        
        RUNTIME_API FVector3 GetLocation() const { return ToVec3(Location); }
        RUNTIME_API FQuat    GetRotation() const { FQuat Q; SIMD::StoreQuat(Q, Rotation); return Q; }
        RUNTIME_API FVector3 GetScale()    const { return ToVec3(Scale); }

        RUNTIME_API void SetLocation(const FVector3& V) { Location = SIMD::VFloat4(V.x, V.y, V.z, 0.0f); }
        RUNTIME_API void SetRotation(const FQuat& Q)    { Rotation = SIMD::LoadQuat(Q); }
        RUNTIME_API void SetScale(const FVector3& V)    { Scale = SIMD::VFloat4(V.x, V.y, V.z, 1.0f); }

        RUNTIME_API FMatrix4 GetMatrix() const
        {
            using namespace SIMD;
            VFloat4 C0, C1, C2;
            QuatToColumns(Rotation, C0, C1, C2);

            FMatrix4 M;
            (C0 * SplatX(Scale)).Store(&M.Cols[0][0]);
            (C1 * SplatY(Scale)).Store(&M.Cols[1][0]);
            (C2 * SplatZ(Scale)).Store(&M.Cols[2][0]);

            const VFloat4 LaneW = _mm_castsi128_ps(_mm_setr_epi32(0, 0, 0, -1));
            Select(LaneW, VFloat4(1.0f), Location).Store(&M.Cols[3][0]);   // (Lx, Ly, Lz, 1)
            return M;
        }

        RUNTIME_API FORCEINLINE FVector3 GetForward() const { return ToVec3(SIMD::QuatRotate(Rotation, SIMD::VFloat4(0.0f, 0.0f, 1.0f, 0.0f))); }
        RUNTIME_API FORCEINLINE FVector3 GetRight()   const { return ToVec3(SIMD::QuatRotate(Rotation, SIMD::VFloat4(1.0f, 0.0f, 0.0f, 0.0f))); }
        RUNTIME_API FORCEINLINE FVector3 GetUp()      const { return ToVec3(SIMD::QuatRotate(Rotation, SIMD::VFloat4(0.0f, 1.0f, 0.0f, 0.0f))); }

        RUNTIME_API FORCEINLINE void SetRotationFromEuler(const FVector3& EulerAngles)
        {
            Rotation = SIMD::LoadQuat(FQuat(Math::Radians(EulerAngles)));
        }

        RUNTIME_API FORCEINLINE void Translate(const FVector3& T)
        {
            Location += SIMD::VFloat4(T.x, T.y, T.z, 0.0f);
        }

        RUNTIME_API FORCEINLINE void Rotate(const FVector3& EulerAngles)
        {
            // Additional * Rotation (apply the new rotation on the outside), matching the scalar transform.
            Rotation = SIMD::QuatMul(SIMD::LoadQuat(FQuat(Math::Radians(EulerAngles))), Rotation);
        }
        
        RUNTIME_API FORCEINLINE void AddYawRadians(float Radians)   { ApplyAxisAngle(SIMD::VFloat4(0.0f, 1.0f, 0.0f, 0.0f), Radians); }
        RUNTIME_API FORCEINLINE void AddPitchRadians(float Radians) { ApplyAxisAngle(SIMD::QuatRotate(Rotation, SIMD::VFloat4(1.0f, 0.0f, 0.0f, 0.0f)), Radians); }
        RUNTIME_API FORCEINLINE void AddRollRadians(float Radians)  { ApplyAxisAngle(SIMD::QuatRotate(Rotation, SIMD::VFloat4(0.0f, 0.0f, 1.0f, 0.0f)), Radians); }

        bool operator==(const VTransform& Other) const
        {
            // All lanes equal (pad lanes match by construction: Location.w=0, Scale.w=1 on both).
            using namespace SIMD;
            return All(CmpEq(Location, Other.Location))
                && All(CmpEq(Rotation, Other.Rotation))
                && All(CmpEq(Scale,    Other.Scale));
        }

        bool operator!=(const VTransform& Other) const { return !(*this == Other); }
        
        RUNTIME_API bool Serialize(FArchive& Ar);

        VTransform operator*(const VTransform& Other) const
        {
            using namespace SIMD;
            VTransform Result;
            Result.Scale    = Scale * Other.Scale;                                  // pad: 1*1 = 1
            Result.Rotation = QuatMul(Rotation, Other.Rotation);
            Result.Location = QuatRotate(Rotation, Scale * Other.Location) + Location;
            return Result;
        }

        VTransform& operator*=(const VTransform& Other)
        {
            *this = operator*(Other);
            return *this;
        }

        RUNTIME_API VTransform Inverse() const
        {
            using namespace SIMD;
            VTransform Inv;
            Inv.Scale    = Reciprocal(Scale);                                       // pad: 1/1 = 1 (never inf)
            Inv.Rotation = QuatConjugate(Rotation);
            Inv.Location = QuatRotate(Inv.Rotation, Inv.Scale * (-Location));
            return Inv;
        }

    private:

        // Rotation = normalize(axisAngle(Axis, Radians) * Rotation), entirely in registers.
        FORCEINLINE void ApplyAxisAngle(SIMD::VFloat4 Axis, float Radians)
        {
            using namespace SIMD;
            Rotation = QuatNormalize(QuatMul(QuatAngleAxis(Axis, Radians), Rotation));
        }

        static FVector3 ToVec3(SIMD::VFloat4 V)
        {
            alignas(16) float B[4];
            V.StoreAligned(B);
            return FVector3(B[0], B[1], B[2]);
        }
    };

}

namespace Lumina
{
    RUNTIME_API void FormatArgument(Fmt::FFormatBuffer& Out, const FTransform& Transform, const Fmt::FFormatSpec& Spec);
}
