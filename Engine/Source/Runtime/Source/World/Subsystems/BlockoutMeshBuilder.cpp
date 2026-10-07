#include "RuntimePCH.h"
#include "BlockoutMeshBuilder.h"

#include "Core/Math/Hash/Hash.h"

#include <cmath>

namespace Lumina
{
    namespace
    {
        constexpr float kMinExtent = 0.001f;
        constexpr float kDegenerateEpsilon = 1e-7f;

        struct FBuildVertex
        {
            FVector3 Position;
            FVector3 Normal;
            FVector2 UV;
        };

        // Every authored field, laid out so the hash never sees the transient build bookkeeping beside them.
        struct FBlockoutParams
        {
            float    SizeX, SizeY, SizeZ;
            float    Thickness;
            float    UVScale;
            int32    RadialSegments;
            int32    RingSegments;
            int32    Steps;
            uint8    Shape;
            uint8    Pivot;
            uint16   Padding;
        };

        FBlockoutParams MakeParams(const SBlockoutComponent& Shape)
        {
            FBlockoutParams P{};
            P.SizeX          = Shape.Size.x;
            P.SizeY          = Shape.Size.y;
            P.SizeZ          = Shape.Size.z;
            P.Thickness      = Shape.Thickness;
            P.UVScale        = Shape.UVScale;
            P.RadialSegments = Shape.RadialSegments;
            P.RingSegments   = Shape.RingSegments;
            P.Steps          = Shape.Steps;
            P.Shape          = (uint8)Shape.Shape;
            P.Pivot          = (uint8)Shape.Pivot;
            P.Padding        = 0;
            return P;
        }

        bool NearlySame(const FVector3& A, const FVector3& B)
        {
            return Math::Abs(A.x - B.x) < kDegenerateEpsilon
                && Math::Abs(A.y - B.y) < kDegenerateEpsilon
                && Math::Abs(A.z - B.z) < kDegenerateEpsilon;
        }

        void AddTri(FBlockoutMeshData& M, const FBuildVertex& A, const FBuildVertex& B, const FBuildVertex& C)
        {
            // Pole stitching collapses a quad onto a line, and meshopt should never see those.
            if (NearlySame(A.Position, B.Position) || NearlySame(B.Position, C.Position) || NearlySame(A.Position, C.Position))
            {
                return;
            }

            const uint32 Base = (uint32)M.Positions.size();
            M.Positions.push_back(A.Position);
            M.Positions.push_back(B.Position);
            M.Positions.push_back(C.Position);
            M.Normals.push_back(A.Normal);
            M.Normals.push_back(B.Normal);
            M.Normals.push_back(C.Normal);
            M.UVs.push_back(A.UV);
            M.UVs.push_back(B.UV);
            M.UVs.push_back(C.UV);
            M.Indices.push_back(Base);
            M.Indices.push_back(Base + 1);
            M.Indices.push_back(Base + 2);
        }

        void AddQuad(FBlockoutMeshData& M, const FBuildVertex& A, const FBuildVertex& B, const FBuildVertex& C, const FBuildVertex& D)
        {
            AddTri(M, A, B, C);
            AddTri(M, A, C, D);
        }

        void PlanarBasis(const FVector3& N, FVector3& OutTangent, FVector3& OutBitangent)
        {
            OutTangent = Math::Abs(N.y) > 0.99f
                       ? FVector3(1.0f, 0.0f, 0.0f)
                       : Math::Normalize(Math::Cross(FVector3(0.0f, 1.0f, 0.0f), N));
            OutBitangent = Math::Cross(N, OutTangent);
        }

        FBuildVertex PlanarVertex(const FVector3& P, const FVector3& N, const FVector3& T, const FVector3& B, float InvUVScale)
        {
            return FBuildVertex{ P, N, FVector2(Math::Dot(P, T) * InvUVScale, Math::Dot(P, B) * InvUVScale) };
        }

        void AddPlanarQuad(FBlockoutMeshData& M, const FVector3& A, const FVector3& B, const FVector3& C, const FVector3& D,
                           const FVector3& N, float InvUVScale)
        {
            FVector3 T, Bt;
            PlanarBasis(N, T, Bt);
            AddQuad(M,
                    PlanarVertex(A, N, T, Bt, InvUVScale),
                    PlanarVertex(B, N, T, Bt, InvUVScale),
                    PlanarVertex(C, N, T, Bt, InvUVScale),
                    PlanarVertex(D, N, T, Bt, InvUVScale));
        }

        void AddPlanarTri(FBlockoutMeshData& M, const FVector3& A, const FVector3& B, const FVector3& C,
                          const FVector3& N, float InvUVScale)
        {
            FVector3 T, Bt;
            PlanarBasis(N, T, Bt);
            AddTri(M,
                   PlanarVertex(A, N, T, Bt, InvUVScale),
                   PlanarVertex(B, N, T, Bt, InvUVScale),
                   PlanarVertex(C, N, T, Bt, InvUVScale));
        }

        // Rings run top to bottom and each carries a duplicated seam vertex, so UVs do not wrap backwards.
        void StitchGrid(FBlockoutMeshData& M, const TVector<FBuildVertex>& Grid, int32 RingCount, int32 Stride, bool bFlip)
        {
            for (int32 Ring = 0; Ring + 1 < RingCount; ++Ring)
            {
                for (int32 Slice = 0; Slice + 1 < Stride; ++Slice)
                {
                    const FBuildVertex& A = Grid[Ring * Stride + Slice];
                    const FBuildVertex& B = Grid[Ring * Stride + Slice + 1];
                    const FBuildVertex& C = Grid[(Ring + 1) * Stride + Slice + 1];
                    const FBuildVertex& D = Grid[(Ring + 1) * Stride + Slice];

                    if (bFlip)
                    {
                        AddQuad(M, A, D, C, B);
                    }
                    else
                    {
                        AddQuad(M, A, B, C, D);
                    }
                }
            }
        }

        void BuildBox(FBlockoutMeshData& M, float HalfX, float Height, float HalfZ, float InvUVScale)
        {
            const float Y0 = 0.0f;
            const float Y1 = Height;

            AddPlanarQuad(M, FVector3(-HalfX, Y1,  HalfZ), FVector3( HalfX, Y1,  HalfZ),
                             FVector3( HalfX, Y1, -HalfZ), FVector3(-HalfX, Y1, -HalfZ), FVector3(0, 1, 0), InvUVScale);

            AddPlanarQuad(M, FVector3(-HalfX, Y0, -HalfZ), FVector3( HalfX, Y0, -HalfZ),
                             FVector3( HalfX, Y0,  HalfZ), FVector3(-HalfX, Y0,  HalfZ), FVector3(0, -1, 0), InvUVScale);

            AddPlanarQuad(M, FVector3(-HalfX, Y0, HalfZ), FVector3( HalfX, Y0, HalfZ),
                             FVector3( HalfX, Y1, HalfZ), FVector3(-HalfX, Y1, HalfZ), FVector3(0, 0, 1), InvUVScale);

            AddPlanarQuad(M, FVector3( HalfX, Y0, -HalfZ), FVector3(-HalfX, Y0, -HalfZ),
                             FVector3(-HalfX, Y1, -HalfZ), FVector3( HalfX, Y1, -HalfZ), FVector3(0, 0, -1), InvUVScale);

            AddPlanarQuad(M, FVector3(HalfX, Y0,  HalfZ), FVector3(HalfX, Y0, -HalfZ),
                             FVector3(HalfX, Y1, -HalfZ), FVector3(HalfX, Y1,  HalfZ), FVector3(1, 0, 0), InvUVScale);

            AddPlanarQuad(M, FVector3(-HalfX, Y0, -HalfZ), FVector3(-HalfX, Y0,  HalfZ),
                             FVector3(-HalfX, Y1,  HalfZ), FVector3(-HalfX, Y1, -HalfZ), FVector3(-1, 0, 0), InvUVScale);
        }

        void BuildPlane(FBlockoutMeshData& M, float HalfX, float HalfZ, float InvUVScale)
        {
            AddPlanarQuad(M, FVector3(-HalfX, 0.0f,  HalfZ), FVector3( HalfX, 0.0f,  HalfZ),
                             FVector3( HalfX, 0.0f, -HalfZ), FVector3(-HalfX, 0.0f, -HalfZ), FVector3(0, 1, 0), InvUVScale);
        }

        void BuildRamp(FBlockoutMeshData& M, float HalfX, float Height, float HalfZ, float InvUVScale)
        {
            AddPlanarQuad(M, FVector3(-HalfX, 0.0f, -HalfZ), FVector3( HalfX, 0.0f, -HalfZ),
                             FVector3( HalfX, 0.0f,  HalfZ), FVector3(-HalfX, 0.0f,  HalfZ), FVector3(0, -1, 0), InvUVScale);

            AddPlanarQuad(M, FVector3( HalfX, 0.0f,   -HalfZ), FVector3(-HalfX, 0.0f,   -HalfZ),
                             FVector3(-HalfX, Height, -HalfZ), FVector3( HalfX, Height, -HalfZ), FVector3(0, 0, -1), InvUVScale);

            const FVector3 SlopeNormal = Math::Normalize(Math::Cross(FVector3(2.0f * HalfX, 0.0f, 0.0f),
                                                                     FVector3(2.0f * HalfX, Height, -2.0f * HalfZ)));
            AddPlanarQuad(M, FVector3(-HalfX, 0.0f,    HalfZ), FVector3( HalfX, 0.0f,    HalfZ),
                             FVector3( HalfX, Height, -HalfZ), FVector3(-HalfX, Height, -HalfZ), SlopeNormal, InvUVScale);

            AddPlanarTri(M, FVector3(HalfX, 0.0f, HalfZ), FVector3(HalfX, 0.0f, -HalfZ), FVector3(HalfX, Height, -HalfZ),
                         FVector3(1, 0, 0), InvUVScale);

            AddPlanarTri(M, FVector3(-HalfX, 0.0f, -HalfZ), FVector3(-HalfX, 0.0f, HalfZ), FVector3(-HalfX, Height, -HalfZ),
                         FVector3(-1, 0, 0), InvUVScale);
        }

        void BuildStairs(FBlockoutMeshData& M, float HalfX, float Height, float HalfZ, int32 Steps, float InvUVScale)
        {
            const float StepDepth  = (2.0f * HalfZ) / float(Steps);
            const float StepHeight = Height / float(Steps);

            AddPlanarQuad(M, FVector3(-HalfX, 0.0f, -HalfZ), FVector3( HalfX, 0.0f, -HalfZ),
                             FVector3( HalfX, 0.0f,  HalfZ), FVector3(-HalfX, 0.0f,  HalfZ), FVector3(0, -1, 0), InvUVScale);

            AddPlanarQuad(M, FVector3( HalfX, 0.0f,   -HalfZ), FVector3(-HalfX, 0.0f,   -HalfZ),
                             FVector3(-HalfX, Height, -HalfZ), FVector3( HalfX, Height, -HalfZ), FVector3(0, 0, -1), InvUVScale);

            for (int32 Step = 0; Step < Steps; ++Step)
            {
                const float ZNear = HalfZ - float(Step) * StepDepth;
                const float ZFar  = ZNear - StepDepth;
                const float YLow  = float(Step) * StepHeight;
                const float YHigh = YLow + StepHeight;

                AddPlanarQuad(M, FVector3(-HalfX, YLow,  ZNear), FVector3( HalfX, YLow,  ZNear),
                                 FVector3( HalfX, YHigh, ZNear), FVector3(-HalfX, YHigh, ZNear), FVector3(0, 0, 1), InvUVScale);

                AddPlanarQuad(M, FVector3(-HalfX, YHigh, ZNear), FVector3( HalfX, YHigh, ZNear),
                                 FVector3( HalfX, YHigh, ZFar),  FVector3(-HalfX, YHigh, ZFar), FVector3(0, 1, 0), InvUVScale);

                AddPlanarQuad(M, FVector3(HalfX, 0.0f,  ZNear), FVector3(HalfX, 0.0f,  ZFar),
                                 FVector3(HalfX, YHigh, ZFar),  FVector3(HalfX, YHigh, ZNear), FVector3(1, 0, 0), InvUVScale);

                AddPlanarQuad(M, FVector3(-HalfX, 0.0f,  ZFar),  FVector3(-HalfX, 0.0f,  ZNear),
                                 FVector3(-HalfX, YHigh, ZNear), FVector3(-HalfX, YHigh, ZFar), FVector3(-1, 0, 0), InvUVScale);
            }
        }

        void AddRadialCap(FBlockoutMeshData& M, float RadiusX, float RadiusZ, float Y, int32 Segments, bool bUpward, float InvUVScale)
        {
            const FVector3 Normal = bUpward ? FVector3(0, 1, 0) : FVector3(0, -1, 0);
            const FVector3 Center(0.0f, Y, 0.0f);

            for (int32 Slice = 0; Slice < Segments; ++Slice)
            {
                const float A0 = (Math::TwoPi<float>() * float(Slice))       / float(Segments);
                const float A1 = (Math::TwoPi<float>() * float(Slice + 1))   / float(Segments);
                const FVector3 P0(RadiusX * std::cos(A0), Y, RadiusZ * std::sin(A0));
                const FVector3 P1(RadiusX * std::cos(A1), Y, RadiusZ * std::sin(A1));

                if (bUpward)
                {
                    AddPlanarTri(M, Center, P1, P0, Normal, InvUVScale);
                }
                else
                {
                    AddPlanarTri(M, Center, P0, P1, Normal, InvUVScale);
                }
            }
        }

        FVector3 RadialNormal(float Angle, float RadiusX, float RadiusZ)
        {
            return Math::Normalize(FVector3(std::cos(Angle) / Math::Max(RadiusX, kMinExtent),
                                            0.0f,
                                            std::sin(Angle) / Math::Max(RadiusZ, kMinExtent)));
        }

        void BuildCylinderSide(FBlockoutMeshData& M, float RadiusX, float RadiusZ, float Y0, float Y1,
                               int32 Segments, bool bInward, float InvUVScale)
        {
            const int32 Stride = Segments + 1;
            const float ArcRadius = (RadiusX + RadiusZ) * 0.5f;

            TVector<FBuildVertex> Grid;
            Grid.resize((size_t)Stride * 2);

            for (int32 Slice = 0; Slice < Stride; ++Slice)
            {
                const float Angle = (Math::TwoPi<float>() * float(Slice)) / float(Segments);
                const float CosA = std::cos(Angle);
                const float SinA = std::sin(Angle);
                const FVector3 Normal = RadialNormal(Angle, RadiusX, RadiusZ) * (bInward ? -1.0f : 1.0f);
                const float U = Angle * ArcRadius * InvUVScale;

                Grid[Slice]          = FBuildVertex{ FVector3(RadiusX * CosA, Y1, RadiusZ * SinA), Normal, FVector2(U, Y1 * InvUVScale) };
                Grid[Stride + Slice] = FBuildVertex{ FVector3(RadiusX * CosA, Y0, RadiusZ * SinA), Normal, FVector2(U, Y0 * InvUVScale) };
            }

            StitchGrid(M, Grid, 2, Stride, bInward);
        }

        void BuildCylinder(FBlockoutMeshData& M, float RadiusX, float Height, float RadiusZ, int32 Segments, float InvUVScale)
        {
            BuildCylinderSide(M, RadiusX, RadiusZ, 0.0f, Height, Segments, false, InvUVScale);
            AddRadialCap(M, RadiusX, RadiusZ, Height, Segments, true,  InvUVScale);
            AddRadialCap(M, RadiusX, RadiusZ, 0.0f,   Segments, false, InvUVScale);
        }

        void BuildCone(FBlockoutMeshData& M, float RadiusX, float Height, float RadiusZ, int32 Segments, float InvUVScale)
        {
            const float ArcRadius = (RadiusX + RadiusZ) * 0.5f;
            const FVector3 Apex(0.0f, Height, 0.0f);

            for (int32 Slice = 0; Slice < Segments; ++Slice)
            {
                const float A0 = (Math::TwoPi<float>() * float(Slice))     / float(Segments);
                const float A1 = (Math::TwoPi<float>() * float(Slice + 1)) / float(Segments);
                const float AMid = (A0 + A1) * 0.5f;

                const FVector3 P0(RadiusX * std::cos(A0), 0.0f, RadiusZ * std::sin(A0));
                const FVector3 P1(RadiusX * std::cos(A1), 0.0f, RadiusZ * std::sin(A1));

                // The slant tilts the side normal off horizontal by the cone's own rise over run.
                const auto SlantNormal = [&](float Angle)
                {
                    const FVector3 Flat = RadialNormal(Angle, RadiusX, RadiusZ);
                    return Math::Normalize(FVector3(Flat.x * Height, ArcRadius, Flat.z * Height));
                };

                const FBuildVertex V0{ P0,   SlantNormal(A0),   FVector2(A0 * ArcRadius * InvUVScale, 0.0f) };
                const FBuildVertex V1{ P1,   SlantNormal(A1),   FVector2(A1 * ArcRadius * InvUVScale, 0.0f) };
                const FBuildVertex VA{ Apex, SlantNormal(AMid), FVector2(AMid * ArcRadius * InvUVScale, Height * InvUVScale) };

                AddTri(M, V0, VA, V1);
            }

            AddRadialCap(M, RadiusX, RadiusZ, 0.0f, Segments, false, InvUVScale);
        }

        void BuildEllipsoidBand(FBlockoutMeshData& M, const FVector3& Center, float RadiusX, float RadiusY, float RadiusZ,
                                float Theta0, float Theta1, int32 Segments, int32 Rings, float InvUVScale)
        {
            const int32 Stride = Segments + 1;
            const int32 RingCount = Rings + 1;
            const float ArcRadius = (RadiusX + RadiusZ) * 0.5f;

            TVector<FBuildVertex> Grid;
            Grid.resize((size_t)Stride * (size_t)RingCount);

            for (int32 Ring = 0; Ring < RingCount; ++Ring)
            {
                const float Theta = Theta0 + (Theta1 - Theta0) * (float(Ring) / float(Rings));
                const float SinT = std::sin(Theta);
                const float CosT = std::cos(Theta);

                for (int32 Slice = 0; Slice < Stride; ++Slice)
                {
                    const float Phi = (Math::TwoPi<float>() * float(Slice)) / float(Segments);
                    const float CosP = std::cos(Phi);
                    const float SinP = std::sin(Phi);

                    const FVector3 Offset(RadiusX * SinT * CosP, RadiusY * CosT, RadiusZ * SinT * SinP);
                    const FVector3 Normal = Math::Normalize(FVector3(SinT * CosP / Math::Max(RadiusX, kMinExtent),
                                                                     CosT        / Math::Max(RadiusY, kMinExtent),
                                                                     SinT * SinP / Math::Max(RadiusZ, kMinExtent)));

                    Grid[(size_t)Ring * Stride + Slice] = FBuildVertex
                    {
                        Center + Offset,
                        Normal,
                        FVector2(Phi * ArcRadius * InvUVScale, Theta * RadiusY * InvUVScale)
                    };
                }
            }

            StitchGrid(M, Grid, RingCount, Stride, false);
        }

        void BuildCapsule(FBlockoutMeshData& M, float RadiusX, float Height, float RadiusZ, int32 Segments, int32 Rings, float InvUVScale)
        {
            // A capsule is round, so the smaller horizontal radius sets the cap height, squashed to fit.
            const float CapRadius = Math::Max(Math::Min(Math::Min(RadiusX, RadiusZ), Height * 0.5f), kMinExtent);
            const float BottomCenter = CapRadius;
            const float TopCenter = Math::Max(Height - CapRadius, CapRadius);

            BuildEllipsoidBand(M, FVector3(0.0f, TopCenter, 0.0f), RadiusX, CapRadius, RadiusZ,
                               0.0f, Math::HalfPi<float>(), Segments, Math::Max(Rings / 2, 1), InvUVScale);

            if (TopCenter > BottomCenter + kMinExtent)
            {
                BuildCylinderSide(M, RadiusX, RadiusZ, BottomCenter, TopCenter, Segments, false, InvUVScale);
            }

            BuildEllipsoidBand(M, FVector3(0.0f, BottomCenter, 0.0f), RadiusX, CapRadius, RadiusZ,
                               Math::HalfPi<float>(), Math::Pi<float>(), Segments, Math::Max(Rings / 2, 1), InvUVScale);
        }

        void BuildTorus(FBlockoutMeshData& M, float MajorRadius, float TubeRadius, int32 MajorSegments, int32 MinorSegments, float InvUVScale)
        {
            const int32 Stride = MinorSegments + 1;
            const int32 RingCount = MajorSegments + 1;

            TVector<FBuildVertex> Grid;
            Grid.resize((size_t)Stride * (size_t)RingCount);

            for (int32 Ring = 0; Ring < RingCount; ++Ring)
            {
                const float U = (Math::TwoPi<float>() * float(Ring)) / float(MajorSegments);
                const float CosU = std::cos(U);
                const float SinU = std::sin(U);

                for (int32 Slice = 0; Slice < Stride; ++Slice)
                {
                    const float V = (Math::TwoPi<float>() * float(Slice)) / float(MinorSegments);
                    const float CosV = std::cos(V);
                    const float SinV = std::sin(V);

                    const FVector3 Normal(CosV * CosU, SinV, CosV * SinU);
                    const FVector3 Position((MajorRadius + TubeRadius * CosV) * CosU,
                                            TubeRadius + TubeRadius * SinV,
                                            (MajorRadius + TubeRadius * CosV) * SinU);

                    Grid[(size_t)Ring * Stride + Slice] = FBuildVertex
                    {
                        Position,
                        Normal,
                        FVector2(U * MajorRadius * InvUVScale, V * TubeRadius * InvUVScale)
                    };
                }
            }

            StitchGrid(M, Grid, RingCount, Stride, false);
        }

        void BuildArch(FBlockoutMeshData& M, float HalfX, float Height, float HalfZ, float Thickness, int32 Steps, float InvUVScale)
        {
            const float OuterX = Math::Max(HalfX, kMinExtent);
            const float OuterY = Math::Max(Height, kMinExtent);
            const float InnerX = Math::Max(OuterX - Thickness, kMinExtent);
            const float InnerY = Math::Max(OuterY - Thickness, kMinExtent);

            const auto Outer = [&](float T, float Z) { return FVector3(OuterX * std::cos(T), OuterY * std::sin(T), Z); };
            const auto Inner = [&](float T, float Z) { return FVector3(InnerX * std::cos(T), InnerY * std::sin(T), Z); };
            const auto BandNormal = [&](float T, float RadX, float RadY)
            {
                return Math::Normalize(FVector3(std::cos(T) / RadX, std::sin(T) / RadY, 0.0f));
            };

            for (int32 Step = 0; Step < Steps; ++Step)
            {
                const float T0 = (Math::Pi<float>() * float(Step))       / float(Steps);
                const float T1 = (Math::Pi<float>() * float(Step + 1))   / float(Steps);

                const FVector3 OuterNormal0 =  BandNormal(T0, OuterX, OuterY);
                const FVector3 OuterNormal1 =  BandNormal(T1, OuterX, OuterY);
                const FVector3 InnerNormal0 = -BandNormal(T0, InnerX, InnerY);
                const FVector3 InnerNormal1 = -BandNormal(T1, InnerX, InnerY);

                const float ArcU0 = T0 * OuterX * InvUVScale;
                const float ArcU1 = T1 * OuterX * InvUVScale;
                const float DepthV0 = -HalfZ * InvUVScale;
                const float DepthV1 =  HalfZ * InvUVScale;

                AddQuad(M,
                        FBuildVertex{ Outer(T0,  HalfZ), OuterNormal0, FVector2(ArcU0, DepthV1) },
                        FBuildVertex{ Outer(T0, -HalfZ), OuterNormal0, FVector2(ArcU0, DepthV0) },
                        FBuildVertex{ Outer(T1, -HalfZ), OuterNormal1, FVector2(ArcU1, DepthV0) },
                        FBuildVertex{ Outer(T1,  HalfZ), OuterNormal1, FVector2(ArcU1, DepthV1) });

                AddQuad(M,
                        FBuildVertex{ Inner(T0, -HalfZ), InnerNormal0, FVector2(ArcU0, DepthV0) },
                        FBuildVertex{ Inner(T0,  HalfZ), InnerNormal0, FVector2(ArcU0, DepthV1) },
                        FBuildVertex{ Inner(T1,  HalfZ), InnerNormal1, FVector2(ArcU1, DepthV1) },
                        FBuildVertex{ Inner(T1, -HalfZ), InnerNormal1, FVector2(ArcU1, DepthV0) });

                AddPlanarQuad(M, Inner(T0, HalfZ), Outer(T0, HalfZ), Outer(T1, HalfZ), Inner(T1, HalfZ),
                              FVector3(0, 0, 1), InvUVScale);

                AddPlanarQuad(M, Outer(T0, -HalfZ), Inner(T0, -HalfZ), Inner(T1, -HalfZ), Outer(T1, -HalfZ),
                              FVector3(0, 0, -1), InvUVScale);
            }

            AddPlanarQuad(M, FVector3(InnerX, 0.0f, -HalfZ), FVector3(OuterX, 0.0f, -HalfZ),
                             FVector3(OuterX, 0.0f,  HalfZ), FVector3(InnerX, 0.0f,  HalfZ), FVector3(0, -1, 0), InvUVScale);

            AddPlanarQuad(M, FVector3(-OuterX, 0.0f, -HalfZ), FVector3(-InnerX, 0.0f, -HalfZ),
                             FVector3(-InnerX, 0.0f,  HalfZ), FVector3(-OuterX, 0.0f,  HalfZ), FVector3(0, -1, 0), InvUVScale);
        }

        void BuildPipe(FBlockoutMeshData& M, float RadiusX, float Height, float RadiusZ, float Thickness, int32 Segments, float InvUVScale)
        {
            const float InnerX = Math::Max(RadiusX - Thickness, kMinExtent);
            const float InnerZ = Math::Max(RadiusZ - Thickness, kMinExtent);

            BuildCylinderSide(M, RadiusX, RadiusZ, 0.0f, Height, Segments, false, InvUVScale);
            BuildCylinderSide(M, InnerX,  InnerZ,  0.0f, Height, Segments, true,  InvUVScale);

            for (int32 Slice = 0; Slice < Segments; ++Slice)
            {
                const float A0 = (Math::TwoPi<float>() * float(Slice))     / float(Segments);
                const float A1 = (Math::TwoPi<float>() * float(Slice + 1)) / float(Segments);

                const FVector3 OuterTop0(RadiusX * std::cos(A0), Height, RadiusZ * std::sin(A0));
                const FVector3 OuterTop1(RadiusX * std::cos(A1), Height, RadiusZ * std::sin(A1));
                const FVector3 InnerTop0(InnerX  * std::cos(A0), Height, InnerZ  * std::sin(A0));
                const FVector3 InnerTop1(InnerX  * std::cos(A1), Height, InnerZ  * std::sin(A1));

                AddPlanarQuad(M, OuterTop0, InnerTop0, InnerTop1, OuterTop1, FVector3(0, 1, 0), InvUVScale);

                const FVector3 OuterBottom0(OuterTop0.x, 0.0f, OuterTop0.z);
                const FVector3 OuterBottom1(OuterTop1.x, 0.0f, OuterTop1.z);
                const FVector3 InnerBottom0(InnerTop0.x, 0.0f, InnerTop0.z);
                const FVector3 InnerBottom1(InnerTop1.x, 0.0f, InnerTop1.z);

                AddPlanarQuad(M, InnerBottom0, OuterBottom0, OuterBottom1, InnerBottom1, FVector3(0, -1, 0), InvUVScale);
            }
        }

        float GeneratedHeight(const SBlockoutComponent& Shape)
        {
            switch (Shape.Shape)
            {
                case EBlockoutShape::Plane: return 0.0f;
                case EBlockoutShape::Torus: return 2.0f * Math::Max(Shape.Thickness, kMinExtent);
                default:                    return Math::Max(Shape.Size.y, kMinExtent);
            }
        }
    }

    void BlockoutMesh::Build(const SBlockoutComponent& Shape, FBlockoutMeshData& Out)
    {
        Out.Reset();

        const float HalfX     = Math::Max(Shape.Size.x, kMinExtent) * 0.5f;
        const float HalfZ     = Math::Max(Shape.Size.z, kMinExtent) * 0.5f;
        const float Height    = Math::Max(Shape.Size.y, kMinExtent);
        const float Thickness = Math::Max(Shape.Thickness, kMinExtent);
        const float InvUVScale = 1.0f / Math::Max(Shape.UVScale, kMinExtent);

        const int32 Radial = Math::Clamp(Shape.RadialSegments, 3, 128);
        const int32 Rings  = Math::Clamp(Shape.RingSegments, 2, 128);
        const int32 Steps  = Math::Clamp(Shape.Steps, 1, 128);

        switch (Shape.Shape)
        {
            case EBlockoutShape::Box:
                BuildBox(Out, HalfX, Height, HalfZ, InvUVScale);
                break;

            case EBlockoutShape::Plane:
                BuildPlane(Out, HalfX, HalfZ, InvUVScale);
                break;

            case EBlockoutShape::Ramp:
                BuildRamp(Out, HalfX, Height, HalfZ, InvUVScale);
                break;

            case EBlockoutShape::Stairs:
                BuildStairs(Out, HalfX, Height, HalfZ, Steps, InvUVScale);
                break;

            case EBlockoutShape::Cylinder:
                BuildCylinder(Out, HalfX, Height, HalfZ, Radial, InvUVScale);
                break;

            case EBlockoutShape::Cone:
                BuildCone(Out, HalfX, Height, HalfZ, Radial, InvUVScale);
                break;

            case EBlockoutShape::Sphere:
                BuildEllipsoidBand(Out, FVector3(0.0f, Height * 0.5f, 0.0f), HalfX, Height * 0.5f, HalfZ,
                                   0.0f, Math::Pi<float>(), Radial, Rings, InvUVScale);
                break;

            case EBlockoutShape::Capsule:
                BuildCapsule(Out, HalfX, Height, HalfZ, Radial, Rings, InvUVScale);
                break;

            case EBlockoutShape::Torus:
                BuildTorus(Out, Math::Max(Math::Min(HalfX, HalfZ) - Thickness, kMinExtent), Thickness, Radial, Rings, InvUVScale);
                break;

            case EBlockoutShape::Arch:
                BuildArch(Out, HalfX, Height, HalfZ, Thickness, Steps, InvUVScale);
                break;

            case EBlockoutShape::Pipe:
                BuildPipe(Out, HalfX, Height, HalfZ, Thickness, Radial, InvUVScale);
                break;
        }

        if (Shape.Pivot == EBlockoutPivot::Center)
        {
            const float Shift = GeneratedHeight(Shape) * 0.5f;
            for (FVector3& Position : Out.Positions)
            {
                Position.y -= Shift;
            }
        }
    }

    uint32 BlockoutMesh::HashParameters(const SBlockoutComponent& Shape)
    {
        const FBlockoutParams Params = MakeParams(Shape);
        return Hash::XXHash::GetHash32(&Params, sizeof(Params));
    }

    void BlockoutMesh::GetLocalBounds(const SBlockoutComponent& Shape, FVector3& OutMin, FVector3& OutMax)
    {
        const float HalfX  = Math::Max(Shape.Size.x, kMinExtent) * 0.5f;
        const float HalfZ  = Math::Max(Shape.Size.z, kMinExtent) * 0.5f;
        const float Height = GeneratedHeight(Shape);
        const float Base   = Shape.Pivot == EBlockoutPivot::Center ? -Height * 0.5f : 0.0f;

        OutMin = FVector3(-HalfX, Base, -HalfZ);
        OutMax = FVector3( HalfX, Base + Height, HalfZ);
    }

    const char* BlockoutMesh::GetShapeName(EBlockoutShape Shape)
    {
        switch (Shape)
        {
            case EBlockoutShape::Box:      return "Box";
            case EBlockoutShape::Plane:    return "Plane";
            case EBlockoutShape::Ramp:     return "Ramp";
            case EBlockoutShape::Stairs:   return "Stairs";
            case EBlockoutShape::Cylinder: return "Cylinder";
            case EBlockoutShape::Cone:     return "Cone";
            case EBlockoutShape::Sphere:   return "Sphere";
            case EBlockoutShape::Capsule:  return "Capsule";
            case EBlockoutShape::Torus:    return "Torus";
            case EBlockoutShape::Arch:     return "Arch";
            case EBlockoutShape::Pipe:     return "Pipe";
        }
        return "Shape";
    }
}
