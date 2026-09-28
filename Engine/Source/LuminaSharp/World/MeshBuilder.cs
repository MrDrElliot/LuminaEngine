using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

/// Flat-shaded, vertex-colored primitives in sRGB, built once as a shared static mesh or committed to a dynamic mesh.
public class MeshBuilder
{
    private readonly List<FVector3> Positions = new();
    private readonly List<FVector3> Normals = new();
    private readonly List<FVector4> Colors = new();
    private readonly List<int> Indices = new();

    /// World units per UV tile along X and Z, for materials that sample a texture over the geometry.
    public float UVScale = 0.25f;

    /// Off when the colors passed in are already linear.
    public bool bSRGBColors = true;

    public int VertexCount => Positions.Count;

    public bool IsEmpty => Indices.Count == 0;

    public static FVector4 Shade(FVector4 Color, float Factor) => new(Color.X * Factor, Color.Y * Factor, Color.Z * Factor, Color.W);

    public static FVector4 SRGBToLinear(FVector4 Color) => new(MathF.Pow(Color.X, 2.2f), MathF.Pow(Color.Y, 2.2f), MathF.Pow(Color.Z, 2.2f), Color.W);

    public void Clear()
    {
        Positions.Clear();
        Normals.Clear();
        Colors.Clear();
        Indices.Clear();
    }

    // Winding is fixed up from Outward, since the engine treats cross(B - A, C - A) facing out as the front.
    public void Quad(FVector3 A, FVector3 B, FVector3 C, FVector3 D, FVector3 Outward, FVector4 Color)
    {
        FVector3 Normal = FVector3.Cross(B - A, C - A);
        if (FVector3.Dot(Normal, Outward) < 0.0f)
        {
            (B, D) = (D, B);
            Normal = -Normal;
        }

        Normal = Normal.NormalizedOr(Outward.NormalizedOr(FVector3.Up));
        int Base = Positions.Count;
        Positions.Add(A);
        Positions.Add(B);
        Positions.Add(C);
        Positions.Add(D);
        for (int Corner = 0; Corner < 4; ++Corner)
        {
            Normals.Add(Normal);
            Colors.Add(Color);
        }

        Indices.Add(Base);
        Indices.Add(Base + 1);
        Indices.Add(Base + 2);
        Indices.Add(Base);
        Indices.Add(Base + 2);
        Indices.Add(Base + 3);
    }

    public void Triangle(FVector3 A, FVector3 B, FVector3 C, FVector3 Outward, FVector4 Color)
    {
        FVector3 Normal = FVector3.Cross(B - A, C - A);
        if (FVector3.Dot(Normal, Outward) < 0.0f)
        {
            (B, C) = (C, B);
            Normal = -Normal;
        }

        Normal = Normal.NormalizedOr(Outward.NormalizedOr(FVector3.Up));
        int Base = Positions.Count;
        Positions.Add(A);
        Positions.Add(B);
        Positions.Add(C);
        for (int Corner = 0; Corner < 3; ++Corner)
        {
            Normals.Add(Normal);
            Colors.Add(Color);
        }

        Indices.Add(Base);
        Indices.Add(Base + 1);
        Indices.Add(Base + 2);
    }

    public void Box(FVector3 Center, FVector3 Half, FVector4 Color) => Box(Center, Half, Color, FQuat.Identity);

    public void Box(FVector3 Center, FVector3 Half, FVector4 Color, FQuat Rotation)
    {
        FVector3 P(float X, float Y, float Z) => Center + Rotation.Rotate(new FVector3(X * Half.X, Y * Half.Y, Z * Half.Z));
        FVector3 N(float X, float Y, float Z) => Rotation.Rotate(new FVector3(X, Y, Z));

        FVector4 Side = Shade(Color, 0.92f);
        FVector4 Bottom = Shade(Color, 0.7f);

        Quad(P(-1, 1, -1), P(1, 1, -1), P(1, 1, 1), P(-1, 1, 1), N(0, 1, 0), Color);
        Quad(P(-1, -1, 1), P(1, -1, 1), P(1, -1, -1), P(-1, -1, -1), N(0, -1, 0), Bottom);
        Quad(P(-1, -1, 1), P(-1, 1, 1), P(1, 1, 1), P(1, -1, 1), N(0, 0, 1), Side);
        Quad(P(1, -1, -1), P(1, 1, -1), P(-1, 1, -1), P(-1, -1, -1), N(0, 0, -1), Side);
        Quad(P(1, -1, 1), P(1, 1, 1), P(1, 1, -1), P(1, -1, -1), N(1, 0, 0), Side);
        Quad(P(-1, -1, -1), P(-1, 1, -1), P(-1, 1, 1), P(-1, -1, 1), N(-1, 0, 0), Side);
    }

    /// A box resting on Base, the way a building sits on the ground.
    public void Block(FVector3 Base, FVector3 Size, FVector4 Color, float YawDegrees = 0.0f)
    {
        FVector3 Half = Size * 0.5f;
        FQuat Rotation = FQuat.FromEuler(0.0f, Mathf.Radians(YawDegrees), 0.0f);
        Box(Base + new FVector3(0.0f, Half.Y, 0.0f), Half, Color, Rotation);
    }

    /// A pitched roof resting on Base, with its ridge along local X.
    public void Gable(FVector3 Base, FVector3 Size, FVector4 Color, float YawDegrees = 0.0f)
    {
        FQuat Rotation = FQuat.FromEuler(0.0f, Mathf.Radians(YawDegrees), 0.0f);
        float HX = Size.X * 0.5f;
        float HZ = Size.Z * 0.5f;
        FVector3 P(float X, float Y, float Z) => Base + Rotation.Rotate(new FVector3(X, Y, Z));
        FVector3 N(float X, float Y, float Z) => Rotation.Rotate(new FVector3(X, Y, Z));

        FVector3 A = P(-HX, 0.0f, -HZ);
        FVector3 B = P(HX, 0.0f, -HZ);
        FVector3 C = P(HX, 0.0f, HZ);
        FVector3 D = P(-HX, 0.0f, HZ);
        FVector3 R0 = P(-HX, Size.Y, 0.0f);
        FVector3 R1 = P(HX, Size.Y, 0.0f);

        Quad(A, R0, R1, B, N(0, HZ, -Size.Y), Color);
        Quad(C, R1, R0, D, N(0, HZ, Size.Y), Shade(Color, 0.9f));
        Triangle(D, R0, A, N(-1, 0, 0), Shade(Color, 0.8f));
        Triangle(B, R1, C, N(1, 0, 0), Shade(Color, 0.8f));
    }

    public void Cylinder(FVector3 Base, float Radius, float Height, FVector4 Color, int Sides = 10)
    {
        Tube(Base, Base + new FVector3(0.0f, Height, 0.0f), Radius, Radius, Color, Sides);
    }

    public void Cone(FVector3 Base, float Radius, float Height, FVector4 Color, int Sides = 10)
    {
        Tube(Base, Base + new FVector3(0.0f, Height, 0.0f), Radius, 0.0f, Color, Sides);
    }

    public void Tube(FVector3 From, FVector3 To, float RadiusFrom, float RadiusTo, FVector4 Color, int Sides = 8, bool bCaps = true)
    {
        FVector3 Axis = (To - From).NormalizedOr(FVector3.Up);
        FVector3 Helper = MathF.Abs(Axis.Y) > 0.9f ? FVector3.Right : FVector3.Up;
        FVector3 U = FVector3.Cross(Axis, Helper).Normalized();
        FVector3 V = FVector3.Cross(Axis, U).Normalized();

        FVector4 SideColor = Shade(Color, 0.93f);
        for (int Side = 0; Side < Sides; ++Side)
        {
            float A0 = Mathf.TwoPi * Side / Sides;
            float A1 = Mathf.TwoPi * (Side + 1) / Sides;
            float AM = (A0 + A1) * 0.5f;
            FVector3 D0 = U * MathF.Cos(A0) + V * MathF.Sin(A0);
            FVector3 D1 = U * MathF.Cos(A1) + V * MathF.Sin(A1);
            FVector3 DM = U * MathF.Cos(AM) + V * MathF.Sin(AM);

            FVector3 B0 = From + D0 * RadiusFrom;
            FVector3 B1 = From + D1 * RadiusFrom;
            FVector3 T0 = To + D0 * RadiusTo;
            FVector3 T1 = To + D1 * RadiusTo;

            if (RadiusTo <= 0.001f)
            {
                Triangle(B0, To, B1, DM, SideColor);
            }
            else
            {
                Quad(B0, T0, T1, B1, DM, SideColor);
            }

            if (bCaps)
            {
                Triangle(From, B0, B1, -Axis, Shade(Color, 0.7f));
                if (RadiusTo > 0.001f)
                {
                    Triangle(To, T1, T0, Axis, Color);
                }
            }
        }
    }

    public void Sphere(FVector3 Center, float Radius, FVector4 Color, int Segments = 8)
    {
        int Rings = Math.Max(3, Segments / 2);
        FVector3 P(float Phi, float Theta) => Center + new FVector3(MathF.Sin(Phi) * MathF.Cos(Theta), MathF.Cos(Phi), MathF.Sin(Phi) * MathF.Sin(Theta)) * Radius;

        for (int Ring = 0; Ring < Rings; ++Ring)
        {
            float Phi0 = Mathf.Pi * Ring / Rings;
            float Phi1 = Mathf.Pi * (Ring + 1) / Rings;
            for (int Seg = 0; Seg < Segments; ++Seg)
            {
                float Theta0 = Mathf.TwoPi * Seg / Segments;
                float Theta1 = Mathf.TwoPi * (Seg + 1) / Segments;
                FVector3 A = P(Phi0, Theta0);
                FVector3 B = P(Phi0, Theta1);
                FVector3 C = P(Phi1, Theta1);
                FVector3 D = P(Phi1, Theta0);
                FVector3 Outward = P((Phi0 + Phi1) * 0.5f, (Theta0 + Theta1) * 0.5f) - Center;
                if (Ring == 0)
                {
                    Triangle(A, D, C, Outward, Color);
                }
                else if (Ring == Rings - 1)
                {
                    Triangle(A, D, B, Outward, Color);
                }
                else
                {
                    Quad(A, D, C, B, Outward, Color);
                }
            }
        }
    }

    /// Builds the geometry once as a static mesh that many entities or foliage instances can share. The world keeps it alive.
    public CStaticMesh? BuildStaticMesh(CWorld World, CMaterialInterface? Material = null, int MaxLODs = 1)
    {
        if (IsEmpty)
        {
            return null;
        }

        (FVector2[] UVs, FVector4[] Linear) = BuildStreams();
        return CMeshLibrary.CreateStaticMesh(World,
            MemoryMarshal.Cast<FVector3, float>(CollectionsMarshal.AsSpan(Positions)),
            MemoryMarshal.Cast<FVector3, float>(CollectionsMarshal.AsSpan(Normals)),
            MemoryMarshal.Cast<FVector2, float>(UVs),
            MemoryMarshal.Cast<FVector4, float>(Linear),
            MemoryMarshal.Cast<int, uint>(CollectionsMarshal.AsSpan(Indices)),
            Material!, MaxLODs);
    }

    /// Replaces the dynamic mesh's geometry with this one, for geometry unique to one entity or rebuilt often.
    public bool CommitTo(SDynamicMeshComponent Mesh, CMaterialInterface? Material = null)
    {
        if (IsEmpty)
        {
            return false;
        }

        (FVector2[] UVs, FVector4[] Linear) = BuildStreams();
        Mesh.ClearMesh();
        Mesh.SetPositions(CollectionsMarshal.AsSpan(Positions));
        Mesh.SetNormals(CollectionsMarshal.AsSpan(Normals));
        Mesh.SetUVs(UVs);
        Mesh.SetColors(Linear);
        Mesh.SetIndices(CollectionsMarshal.AsSpan(Indices));
        Mesh.AddSection(0, 0, Indices.Count);
        if (Material is not null)
        {
            Mesh.SetMaterialAtSlot(Material, 0);
        }

        return Mesh.Commit();
    }

    private (FVector2[] UVs, FVector4[] Linear) BuildStreams()
    {
        FVector2[] UVs = new FVector2[Positions.Count];
        FVector4[] Linear = new FVector4[Colors.Count];
        for (int Index = 0; Index < Positions.Count; ++Index)
        {
            UVs[Index] = new FVector2(Positions[Index].X * UVScale, Positions[Index].Z * UVScale);
            Linear[Index] = bSRGBColors ? SRGBToLinear(Colors[Index]) : Colors[Index];
        }

        return (UVs, Linear);
    }
}
