#include "Core/Math/Transform.h"
#include "Core/Math/SIMD/VQuat1.h"
#include "GoogleTest/include/gtest/gtest.h"

TEST(TransformTests, DefaultTransformIsIdentity)
{
    Lumina::FTransform Transform{};

    EXPECT_FLOAT_EQ(Transform.GetLocation().x, 0.0f);
    EXPECT_FLOAT_EQ(Transform.GetLocation().y, 0.0f);
    EXPECT_FLOAT_EQ(Transform.GetLocation().z, 0.0f);

    EXPECT_FLOAT_EQ(Transform.GetScale().x, 1.0f);
    EXPECT_FLOAT_EQ(Transform.GetScale().y, 1.0f);
    EXPECT_FLOAT_EQ(Transform.GetScale().z, 1.0f);
}

// The single-quat SSE kernels must match the scalar FQuat math lane for lane.
TEST(SIMDQuat, MatchesScalarQuat)
{
    using namespace Lumina;

    auto MakeQ = [](float Deg, FVector3 Axis)
    {
        return Math::FromAxisAngle(Math::Normalize(Axis), Math::Radians(Deg));
    };

    const FQuat Qs[] = {
        MakeQ(37.0f,  FVector3(0.2f, 1.0f, 0.3f)),
        MakeQ(-64.0f, FVector3(1.0f, 0.1f, -0.5f)),
        MakeQ(180.0f, FVector3(0.0f, 0.0f, 1.0f)),
        FQuat(1.0f, 0.0f, 0.0f, 0.0f), // identity (w,x,y,z ctor)
    };
    const FVector3 Vs[] = { {1.5f, -2.0f, 0.7f}, {0.0f, 0.0f, 1.0f}, {-3.0f, 4.0f, 0.0f} };

    auto Extract = [](SIMD::VFloat4 V)
    {
        alignas(16) float B[4];
        V.StoreAligned(B);
        return FQuat(B[3], B[0], B[1], B[2]); // (w,x,y,z) ctor; lanes are x,y,z,w
    };

    for (const FQuat& A : Qs)
    {
        for (const FQuat& Bq : Qs)
        {
            // Hamilton product.
            const FQuat Ref  = A * Bq;
            const FQuat Simd = Extract(SIMD::QuatMul(SIMD::LoadQuat(A), SIMD::LoadQuat(Bq)));
            EXPECT_NEAR(Simd.x, Ref.x, 1e-5f);
            EXPECT_NEAR(Simd.y, Ref.y, 1e-5f);
            EXPECT_NEAR(Simd.z, Ref.z, 1e-5f);
            EXPECT_NEAR(Simd.w, Ref.w, 1e-5f);
        }

        // Conjugate.
        const FQuat ConjRef  = Math::Conjugate(A);
        const FQuat ConjSimd = Extract(SIMD::QuatConjugate(SIMD::LoadQuat(A)));
        EXPECT_NEAR(ConjSimd.x, ConjRef.x, 1e-5f);
        EXPECT_NEAR(ConjSimd.w, ConjRef.w, 1e-5f);

        // Rotate a vector.
        for (const FVector3& V : Vs)
        {
            const FVector3 Ref = A * V; // FQuat * FVector3 rotate
            alignas(16) float B[4];
            SIMD::QuatRotate(SIMD::LoadQuat(A), SIMD::VFloat4(V.x, V.y, V.z, 0.0f)).StoreAligned(B);
            EXPECT_NEAR(B[0], Ref.x, 1e-5f);
            EXPECT_NEAR(B[1], Ref.y, 1e-5f);
            EXPECT_NEAR(B[2], Ref.z, 1e-5f);
        }

        // Axis-angle quat (the in-register AddYaw/Pitch/Roll core) must match scalar Math::AngleAxis.
        for (const FVector3& V : Vs)
        {
            const FVector3 Axis = Math::Normalize(V);
            const FQuat RefQ  = Math::AngleAxis(0.6f, Axis);
            const FQuat SimdQ = Extract(SIMD::QuatAngleAxis(SIMD::VFloat4(Axis.x, Axis.y, Axis.z, 0.0f), 0.6f));
            EXPECT_NEAR(SimdQ.x, RefQ.x, 1e-5f);
            EXPECT_NEAR(SimdQ.y, RefQ.y, 1e-5f);
            EXPECT_NEAR(SimdQ.z, RefQ.z, 1e-5f);
            EXPECT_NEAR(SimdQ.w, RefQ.w, 1e-5f);
        }

        // Quat -> rotation-matrix columns must match scalar ToMatrix3 (the SIMD core of GetMatrix).
        {
            const FMatrix3 RefM = Math::ToMatrix3(A);
            SIMD::VFloat4 C0, C1, C2;
            SIMD::QuatToColumns(SIMD::LoadQuat(A), C0, C1, C2);
            alignas(16) float Cb[3][4];
            C0.StoreAligned(Cb[0]); C1.StoreAligned(Cb[1]); C2.StoreAligned(Cb[2]);
            for (int Col = 0; Col < 3; ++Col)
            {
                for (int Row = 0; Row < 3; ++Row)
                {
                    EXPECT_NEAR(Cb[Col][Row], RefM[Col][Row], 1e-5f);
                }
                EXPECT_NEAR(Cb[Col][3], 0.0f, 1e-6f); // w lane padded to 0
            }
        }
    }
}
