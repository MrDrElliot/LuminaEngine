using System;
using Lumina;
using LuminaSharp;

namespace Game;

/// <summary>
/// Procedurally builds a (P,Q) torus knot into an SDynamicMeshComponent on the same entity.
///
/// Attach to an entity that has a Dynamic Mesh component, press Play, and it builds on OnReady.
/// Turn on Animate to rebuild every frame, which is the interesting case for measuring Commit():
/// watch the "Dynamic Mesh" stat it logs, then try MaxLODs = 1 and GenerateTangents = false and
/// compare. Those two are the knobs that dominate a rebuild.
/// </summary>
public sealed class TorusKnotMesh : EntityScript
{
    [Property(Tooltip = "Times the tube winds around the torus axis. 2,3 is the classic trefoil.")]
    public int P = 2;

    [Property(Tooltip = "Times the tube winds through the torus hole.")]
    public int Q = 3;

    [Property(Tooltip = "Segments along the knot curve. Vertex count is this times TubeSegments.")]
    public int CurveSegments = 256;

    [Property(Tooltip = "Segments around the tube cross-section.")]
    public int TubeSegments = 32;

    [Property(Tooltip = "Overall size of the knot.")]
    public float Scale = 100.0f;

    [Property(Tooltip = "Radius of the tube itself, in the same units as Scale.")]
    public float TubeRadius = 22.0f;

    [Property(Tooltip = "Rebuild and re-Commit every frame. This is the stress test.")]
    public bool Animate = false;

    [Property(Tooltip = "How fast the animated version writhes and pulses.")]
    public float AnimSpeed = 1.0f;

    [Property(Tooltip = "Let the engine derive smooth normals instead of supplying analytic ones. " +
                        "On exercises the parallel normal path; off is cheaper and exact.")]
    public bool DeriveNormals = true;

    [Property(Tooltip = "LOD levels to build per Commit. 1 is much faster and fine for a rebuilt mesh.")]
    public int MaxLODs = 6;

    [Property(Tooltip = "Generate MikkTSpace tangents. Off is a large saving; only matters if the " +
                        "material samples a normal map.")]
    public bool GenerateTangents = true;

    private SDynamicMeshComponent? Mesh;

    // Reused across rebuilds so an animated knot does not allocate six arrays per frame.
    private FVector3[] Positions = Array.Empty<FVector3>();
    private FVector3[] Normals   = Array.Empty<FVector3>();
    private FVector2[] UVs       = Array.Empty<FVector2>();
    private FVector4[] Colors    = Array.Empty<FVector4>();
    private int[]      Indices   = Array.Empty<int>();
    private int        AllocatedForU = -1;
    private int        AllocatedForV = -1;

    private float Time;

    public override void OnReady()
    {
        Mesh = Registry.TryGet<SDynamicMeshComponent>(Entity);
        if (Mesh == null)
        {
            Debug.LogError("TorusKnotMesh: no Dynamic Mesh component on this entity. Add one and replay.");
            return;
        }

        Rebuild();

        Debug.Log($"TorusKnotMesh: built {Mesh.GetVertexCount()} verts / {Mesh.GetTriangleCount()} tris " +
                  $"(MaxLODs={Mesh.MaxLODs}, Tangents={Mesh.bGenerateTangents}).");
    }

    public override void OnUpdate(float DeltaTime)
    {
        if (Mesh == null || !Animate)
        {
            return;
        }

        Time += DeltaTime * AnimSpeed;
        Rebuild();
    }

    private void Rebuild()
    {
        // Guard the inputs: a zero or negative count would produce an empty stream and Commit would
        // just reject it, and huge counts are an easy way to accidentally hang the editor.
        int SegU = Math.Clamp(CurveSegments, 8, 4096);
        int SegV = Math.Clamp(TubeSegments, 3, 256);

        EnsureCapacity(SegU, SegV);

        // Animated variants: the tube swells along its length and the whole knot slowly writhes.
        float Writhe = Animate ? Time : 0.0f;
        float Pulse  = Animate ? 1.0f + 0.25f * MathF.Sin(Time * 2.0f) : 1.0f;

        const float Tau = MathF.PI * 2.0f;

        for (int i = 0; i < SegU; ++i)
        {
            float u  = (float)i / SegU;
            float t  = u * Tau;

            // Curve point and a finite-difference tangent. A circular cross-section is rotationally
            // symmetric, so an arbitrary (possibly twisting) frame is invisible in the geometry --
            // it only shows up in how the UVs wind, which is fine here.
            FVector3 Center  = CurvePoint(t, Writhe);
            FVector3 Ahead   = CurvePoint(t + 0.01f, Writhe);
            FVector3 Tangent = Normalize(Ahead - Center);

            FVector3 Ref    = MathF.Abs(Tangent.Y) < 0.9f ? new FVector3(0, 1, 0) : new FVector3(1, 0, 0);
            FVector3 Side   = Normalize(Cross(Tangent, Ref));
            FVector3 Up     = Cross(Side, Tangent);

            // Swell travelling along the knot, so an animated build visibly deforms rather than
            // just spinning (a spin would not prove the mesh is actually being rebuilt).
            float Swell = Pulse * (1.0f + 0.35f * MathF.Sin(t * 3.0f - Time * 4.0f));
            float R     = TubeRadius * Swell;

            for (int j = 0; j < SegV; ++j)
            {
                float v = (float)j / SegV;
                float a = v * Tau;

                FVector3 Offset = (Side * MathF.Cos(a) + Up * MathF.Sin(a));
                int Index = i * SegV + j;

                Positions[Index] = Center + Offset * R;
                Normals[Index]   = Offset;                    // exact for a circular tube
                UVs[Index]       = new FVector2(u * P, v);
                Colors[Index]    = Rainbow(u + Time * 0.05f);
            }
        }

        // Two-directional wrap: the knot closes on itself and so does each ring.
        int Tri = 0;
        for (int i = 0; i < SegU; ++i)
        {
            int iNext = (i + 1) % SegU;
            for (int j = 0; j < SegV; ++j)
            {
                int jNext = (j + 1) % SegV;

                int A = i     * SegV + j;
                int B = iNext * SegV + j;
                int C = iNext * SegV + jNext;
                int D = i     * SegV + jNext;

                Indices[Tri++] = A; Indices[Tri++] = B; Indices[Tri++] = C;
                Indices[Tri++] = A; Indices[Tri++] = C; Indices[Tri++] = D;
            }
        }

        Mesh!.MaxLODs           = Math.Clamp(MaxLODs, 1, 6);
        Mesh.bGenerateTangents  = GenerateTangents;

        Mesh.SetPositions(Positions);
        Mesh.SetUVs(UVs);
        Mesh.SetColors(Colors);
        Mesh.SetIndices(Indices);

        // Skipping the normal stream makes Commit derive smooth normals itself, which is the path
        // worth exercising; supplying them is exact and cheaper.
        if (!DeriveNormals)
        {
            Mesh.SetNormals(Normals);
        }

        Mesh.Commit();
    }

    private void EnsureCapacity(int SegU, int SegV)
    {
        if (AllocatedForU == SegU && AllocatedForV == SegV)
        {
            return;
        }

        int VertexCount = SegU * SegV;
        Positions = new FVector3[VertexCount];
        Normals   = new FVector3[VertexCount];
        UVs       = new FVector2[VertexCount];
        Colors    = new FVector4[VertexCount];
        Indices   = new int[SegU * SegV * 6];

        AllocatedForU = SegU;
        AllocatedForV = SegV;
    }

    // Standard (P,Q) torus knot. Writhe rotates the Q-winding over time so the shape churns.
    private FVector3 CurvePoint(float t, float Writhe)
    {
        float Pt = P * t;
        float Qt = Q * t + Writhe;

        float R = 2.0f + MathF.Cos(Qt);
        return new FVector3(
            R * MathF.Cos(Pt),
            R * MathF.Sin(Pt),
            -MathF.Sin(Qt)) * (Scale / 3.0f);
    }

    // Cheap 6-band hue sweep; linear RGB, so it stays saturated through the tonemapper.
    private static FVector4 Rainbow(float T)
    {
        T -= MathF.Floor(T);
        float H = T * 6.0f;
        float X = 1.0f - MathF.Abs(H % 2.0f - 1.0f);

        return (int)H switch
        {
            0 => new FVector4(1, X, 0, 1),
            1 => new FVector4(X, 1, 0, 1),
            2 => new FVector4(0, 1, X, 1),
            3 => new FVector4(0, X, 1, 1),
            4 => new FVector4(X, 0, 1, 1),
            _ => new FVector4(1, 0, X, 1),
        };
    }

    private static FVector3 Cross(FVector3 A, FVector3 B) => new(
        A.Y * B.Z - A.Z * B.Y,
        A.Z * B.X - A.X * B.Z,
        A.X * B.Y - A.Y * B.X);

    private static FVector3 Normalize(FVector3 V)
    {
        float Len = MathF.Sqrt(V.X * V.X + V.Y * V.Y + V.Z * V.Z);
        return Len > 1e-6f ? V * (1.0f / Len) : new FVector3(0, 1, 0);
    }
}
