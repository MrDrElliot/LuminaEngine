#pragma once

#include "Containers/Array.h"
#include "Core/Math/Math.h"
#include "Renderer/Vertex.h"


namespace Lumina::PrimitiveMeshes
{
    inline void GenerateCube(TVector<FVertex>& OutVertices, TVector<uint32>& OutIndices)
    {
        const FVector3 normals[] =
        {
            { 0,  0,  1}, { 0,  0, -1}, {-1,  0,  0},
            { 1,  0,  0}, { 0,  1,  0}, { 0, -1,  0}
        };

        const FVector3 positions[24] =
        {
            // Front
            {-1, -1,  1}, {1, -1,  1}, {1, 1,  1}, {-1, 1,  1},
            // Back
            {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1},
            // Left
            {-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1},
            // Right
            {1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1},
            // Top
            {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1},
            // Bottom
            {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1},
        };

        const FVector2 uvs[4] =
        {
            {0, 0}, {1, 0}, {1, 1}, {0, 1}
        };

        OutVertices.clear();
        OutIndices.clear();
        OutVertices.reserve(24);

        for (int face = 0; face < 6; ++face)
        {
            for (int i = 0; i < 4; ++i)
            {
                int idx = face * 4 + i;
            
                FVertex vertex;
                vertex.Position = positions[idx];
                vertex.Normal = PackNormal(normals[face]);
                vertex.Tangent = 0; // MikkTSpace fills this in GenerateMeshlets; zero so dedup byte-compare works.
                vertex.UV = Math::PackHalf2x16(uvs[i]);
                vertex.Color = 0xFFFFFFFF; // White

                OutVertices.push_back(vertex);
            }

            uint32 base = face * 4;
            OutIndices.push_back(base + 0);
            OutIndices.push_back(base + 1);
            OutIndices.push_back(base + 2);
            OutIndices.push_back(base + 2);
            OutIndices.push_back(base + 3);
            OutIndices.push_back(base + 0);
        }
    }

    inline void GeneratePlane(TVector<FVertex>& OutVertices, TVector<uint32>& OutIndices)
    {
        const FVector3 normal = { 0, 0, 1 };
        const FVector3 positions[4] =
        {
            {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}
        };

        const FVector2 uvs[4] = { {0,0}, {1,0}, {1,1}, {0,1} };

        OutVertices.clear();
        OutIndices.clear();
        OutVertices.reserve(4);
        OutIndices.reserve(6);

        for (int i = 0; i < 4; ++i)
        {
            FVertex v;
            v.Position = positions[i];
            v.Normal = PackNormal(normal);
            v.Tangent = 0;
            v.UV     = Math::PackHalf2x16(uvs[i]);
            v.Color = 0xFFFFFFFF;
            OutVertices.push_back(v);
        }

        OutIndices = { 0, 1, 2, 2, 3, 0 };
    }

    inline void GenerateSphere(TVector<FVertex>& OutVertices, TVector<uint32>& OutIndices, int LatitudeSegments = 16, int LongitudeSegments = 32)
    {
        OutVertices.clear();
        OutIndices.clear();

        for (int y = 0; y <= LatitudeSegments; ++y)
        {
            float v = (float)y / LatitudeSegments;
            float phi = v * Math::Pi<float>();

            for (int x = 0; x <= LongitudeSegments; ++x)
            {
                float u = (float)x / LongitudeSegments;
                float theta = u * Math::TwoPi<float>();

                FVector3 pos =
                {
                    std::sin(phi) * std::cos(theta),
                    std::cos(phi),
                    std::sin(phi) * std::sin(theta)
                };

                FVertex vert;
                vert.Position = pos;
                vert.Normal = PackNormal(Math::Normalize(pos));
                vert.Tangent = 0;
                vert.UV     = Math::PackHalf2x16(FVector2(u, v));
                vert.Color = 0xFFFFFFFF;
                OutVertices.push_back(vert);
            }
        }

        for (int y = 0; y < LatitudeSegments; ++y)
        {
            for (int x = 0; x < LongitudeSegments; ++x)
            {
                uint32 i0 = y * (LongitudeSegments + 1) + x;
                uint32 i1 = i0 + LongitudeSegments + 1;

                OutIndices.push_back(i0);
                OutIndices.push_back(i0 + 1);
                OutIndices.push_back(i1);

                // Second triangle - reversed
                OutIndices.push_back(i0 + 1);
                OutIndices.push_back(i1 + 1);
                OutIndices.push_back(i1);
            }
        }
    }

    inline void GenerateCylinder(TVector<FVertex>& OutVertices, TVector<uint32>& OutIndices, int Segments = 32)
    {
        OutVertices.clear();
        OutIndices.clear();
    
        float halfHeight = 1.0f;
    
        // Side vertices
        for (int i = 0; i <= Segments; ++i)
        {
            float u = (float)i / Segments;
            float theta = u * Math::TwoPi<float>();
            FVector3 dir = { std::cos(theta), 0, std::sin(theta) };
    
            for (int j = 0; j < 2; ++j)
            {
                FVertex v;
                v.Position = { dir.x, j ? halfHeight : -halfHeight, dir.z };
                v.Normal = PackNormal(Math::Normalize(dir));
                v.Tangent = 0;
                v.UV = Math::PackHalf2x16(FVector2(u, j));
                v.Color = 0xFFFFFFFF;
                OutVertices.push_back(v);
            }
        }
    
        // Side indices
        for (int i = 0; i < Segments; ++i)
        {
            uint32 base = i * 2;
            OutIndices.push_back(base);
            OutIndices.push_back(base + 1);
            OutIndices.push_back(base + 2);
            OutIndices.push_back(base + 2);
            OutIndices.push_back(base + 1);
            OutIndices.push_back(base + 3);
        }
    
        for (int cap = 0; cap < 2; ++cap)
        {
            float y = cap ? halfHeight : -halfHeight;
            FVector3 n = { 0, cap ? 1 : -1, 0 };
    
            // center vertex
            FVertex center;
            center.Position = { 0, y, 0 };
            center.Normal = PackNormal(n);
            center.Tangent = 0;
            center.UV = Math::PackHalf2x16(FVector2(32768, 32768));
            center.Color = 0xFFFFFFFF;
            OutVertices.push_back(center);
            uint32 centerIdx = (uint32)OutVertices.size() - 1;

            for (int i = 0; i <= Segments; ++i)
            {
                float u = (float)i / Segments;
                float theta = u * Math::TwoPi<float>();
                FVector3 dir = { std::cos(theta), 0, std::sin(theta) };

                FVertex v;
                v.Position = { dir.x, y, dir.z };
                v.Normal = PackNormal(n);
                v.Tangent = 0;
                v.UV    = Math::PackHalf2x16(FVector2(u, cap));
                v.Color = 0xFFFFFFFF;
                OutVertices.push_back(v);
    
                if (i > 0)
                {
                    uint32 a = centerIdx;
                    uint32 b = centerIdx + i;
                    uint32 c = centerIdx + i + 1;
                    if (cap)
                    {
                        OutIndices.insert(OutIndices.end(), { a, c, b });
                    }
                    else
                    {
                        OutIndices.insert(OutIndices.end(), { a, b, c });
                    }
                }
            }
        }
    }

    inline void GenerateCone(TVector<FVertex>& OutVertices, TVector<uint32>& OutIndices, int Segments = 32)
    {
        OutVertices.clear();
        OutIndices.clear();

        const float halfHeight = 1.0f;
        const float radius     = 1.0f;
        const float H          = halfHeight * 2.0f;
        
        for (int i = 0; i < Segments; ++i)
        {
            float u0 = (float)i / Segments;
            float u1 = (float)(i + 1) / Segments;
            float theta0 = u0 * Math::TwoPi<float>();
            float theta1 = u1 * Math::TwoPi<float>();
            float thetaMid = (theta0 + theta1) * 0.5f;

            FVector3 normal0   = Math::Normalize(FVector3(H * std::cos(theta0),   radius, H * std::sin(theta0)));
            FVector3 normal1   = Math::Normalize(FVector3(H * std::cos(theta1),   radius, H * std::sin(theta1)));
            FVector3 normalTip = Math::Normalize(FVector3(H * std::cos(thetaMid), radius, H * std::sin(thetaMid)));

            FVertex v0;
            v0.Position = { std::cos(theta0) * radius, -halfHeight, std::sin(theta0) * radius };
            v0.Normal   = PackNormal(normal0);
            v0.Tangent  = 0;
            v0.UV       = Math::PackHalf2x16(FVector2(u0, 0));
            v0.Color    = 0xFFFFFFFF;

            FVertex v1;
            v1.Position = { std::cos(theta1) * radius, -halfHeight, std::sin(theta1) * radius };
            v1.Normal   = PackNormal(normal1);
            v1.Tangent  = 0;
            v1.UV       = Math::PackHalf2x16(FVector2(u1, 0));
            v1.Color    = 0xFFFFFFFF;

            FVertex vTip;
            vTip.Position = { 0, halfHeight, 0 };
            vTip.Normal   = PackNormal(normalTip);
            vTip.Tangent  = 0;
            vTip.UV       = Math::PackHalf2x16(FVector2((u0 + u1) * 0.5f, 1));
            vTip.Color    = 0xFFFFFFFF;

            uint32 base = (uint32)OutVertices.size();
            OutVertices.push_back(v0);
            OutVertices.push_back(v1);
            OutVertices.push_back(vTip);

            OutIndices.push_back(base + 0);
            OutIndices.push_back(base + 2);
            OutIndices.push_back(base + 1);
        }

        // Bottom cap
        const FVector3 capNormal = { 0, -1, 0 };

        FVertex centerVert;
        centerVert.Position = { 0, -halfHeight, 0 };
        centerVert.Normal   = PackNormal(capNormal);
        centerVert.Tangent  = 0;
        centerVert.UV       = Math::PackHalf2x16(FVector2(0.5f, 0.5f));
        centerVert.Color    = 0xFFFFFFFF;

        uint32 centerIdx = (uint32)OutVertices.size();
        OutVertices.push_back(centerVert);

        for (int i = 0; i <= Segments; ++i)
        {
            float u = (float)i / Segments;
            float theta = u * Math::TwoPi<float>();
            FVector3 dir = { std::cos(theta), 0, std::sin(theta) };

            FVertex v;
            v.Position = { dir.x * radius, -halfHeight, dir.z * radius };
            v.Normal   = PackNormal(capNormal);
            v.Tangent  = 0;
            v.UV       = Math::PackHalf2x16(FVector2(0.5f + 0.5f * dir.x, 0.5f + 0.5f * dir.z));
            v.Color    = 0xFFFFFFFF;
            OutVertices.push_back(v);

            if (i > 0)
            {
                uint32 a = centerIdx;
                uint32 b = centerIdx + i;
                uint32 c = centerIdx + i + 1;
                OutIndices.insert(OutIndices.end(), { a, b, c });
            }
        }
    }
    
    inline void GenerateCapsule(TVector<FVertex>& OutVertices, TVector<uint32>& OutIndices, int Segments = 32, float Radius = 0.5f, float HalfHeight = 1.0f)
    {
        OutVertices.clear();
        OutIndices.clear();

        // Default radius/halfHeight gives a 2-unit-tall, 1-unit-wide capsule (1-unit cylinder section).
        // The old (1, 1) defaults collapsed the cylinder to zero, rendering as a sphere.
        const float radius = Radius;
        const float halfHeight = HalfHeight;

        const float cylinderHalf = Math::Max(0.0f, halfHeight - radius);
        const int hemiSegments = Segments;
        const int circleSegments = Segments;
    
        auto addVertex = [&](const FVector3& pos, const FVector3& n, const FVector2& uv)
        {
            FVertex v;
            v.Position = pos;
            v.Normal   = PackNormal(n);
            v.Tangent  = 0;
            v.UV       = Math::PackHalf2x16(uv);
            v.Color    = 0xFFFFFFFF;
            OutVertices.push_back(v);
        };
            
        for (int i = 0; i < circleSegments; ++i)
        {
            float u0 = (float)i / circleSegments;
            float u1 = (float)(i + 1) / circleSegments;
    
            float t0 = u0 * Math::TwoPi<float>();
            float t1 = u1 * Math::TwoPi<float>();
    
            FVector3 p0 = { std::cos(t0) * radius, -cylinderHalf, std::sin(t0) * radius };
            FVector3 p1 = { std::cos(t1) * radius, -cylinderHalf, std::sin(t1) * radius };
            FVector3 p2 = { std::cos(t1) * radius,  cylinderHalf, std::sin(t1) * radius };
            FVector3 p3 = { std::cos(t0) * radius,  cylinderHalf, std::sin(t0) * radius };
    
            FVector3 n0 = Math::Normalize(FVector3(p0.x, 0, p0.z));
            FVector3 n1 = Math::Normalize(FVector3(p1.x, 0, p1.z));
    
            uint32 base = (uint32)OutVertices.size();
    
            addVertex(p0, n0, { u0, 0 });
            addVertex(p1, n1, { u1, 0 });
            addVertex(p2, n1, { u1, 1 });
            addVertex(p3, n0, { u0, 1 });
    
            OutIndices.insert(OutIndices.end(),
            {
                base + 0, base + 2, base + 1,
                base + 0, base + 3, base + 2
            });
        }
            
        auto buildHemisphere = [&](bool top)
        {
            float sign = top ? 1.0f : -1.0f;
            FVector3 centerOffset = { 0, sign * cylinderHalf, 0 };
    
            for (int y = 0; y < hemiSegments; ++y)
            {
                float v0 = (float)y / hemiSegments;
                float v1 = (float)(y + 1) / hemiSegments;
    
                float phi0 = v0 * (Math::HalfPi<float>());
                float phi1 = v1 * (Math::HalfPi<float>());
    
                for (int x = 0; x < circleSegments; ++x)
                {
                    float u0 = (float)x / circleSegments;
                    float u1 = (float)(x + 1) / circleSegments;
    
                    float t0 = u0 * Math::TwoPi<float>();
                    float t1 = u1 * Math::TwoPi<float>();
    
                    FVector3 p00 = {
                        std::cos(t0) * std::cos(phi0),
                        std::sin(phi0) * sign,
                        std::sin(t0) * std::cos(phi0)
                    };
    
                    FVector3 p10 = {
                        std::cos(t1) * std::cos(phi0),
                        std::sin(phi0) * sign,
                        std::sin(t1) * std::cos(phi0)
                    };
    
                    FVector3 p01 = {
                        std::cos(t0) * std::cos(phi1),
                        std::sin(phi1) * sign,
                        std::sin(t0) * std::cos(phi1)
                    };
    
                    FVector3 p11 = {
                        std::cos(t1) * std::cos(phi1),
                        std::sin(phi1) * sign,
                        std::sin(t1) * std::cos(phi1)
                    };
    
                    FVector3 n00 = Math::Normalize(p00);
                    FVector3 n10 = Math::Normalize(p10);
                    FVector3 n01 = Math::Normalize(p01);
                    FVector3 n11 = Math::Normalize(p11);
    
                    uint32 base = (uint32)OutVertices.size();
    
                    addVertex(p00 * radius + centerOffset, n00, { u0, v0 });
                    addVertex(p10 * radius + centerOffset, n10, { u1, v0 });
                    addVertex(p11 * radius + centerOffset, n11, { u1, v1 });
                    addVertex(p01 * radius + centerOffset, n01, { u0, v1 });
    
                    if (top)
                    {
                        OutIndices.insert(OutIndices.end(),
                        {
                            base + 0, base + 2, base + 1,
                            base + 0, base + 3, base + 2
                        });
                    }
                    else
                    {
                        OutIndices.insert(OutIndices.end(),
                        {
                            base + 0, base + 1, base + 2,
                            base + 0, base + 2, base + 3
                        });
                    }
                }
            }
        };
    
        buildHemisphere(true);
        buildHemisphere(false);
    }
}
