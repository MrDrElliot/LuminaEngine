using System;
using System.Collections.Generic;
using Lumina;
using LuminaSharp;

namespace Game;

/// <summary>
/// Builds a procedural solar system out of SDynamicMeshComponent spheres and animates it.
///
/// Attach to any entity, press Play. On OnReady it generates a sun at this entity's location, a ring of
/// planets orbiting it, and a few moons around each planet -- every body is its own entity carrying a
/// dynamic mesh built from scratch. OnUpdate then drives every body's orbit and axial spin.
///
/// Bodies are FLAT (no parenting): a moon's world position is computed from its planet's position the same
/// frame. That keeps the whole simulation one pass of pure math with no dependence on transform-hierarchy
/// propagation order, and it means orbit and spin stay completely independent of each other.
/// </summary>
public sealed class UniverseGenerator : EntityScript
{
    [Property(Tooltip = "Planets orbiting the sun.")]
    public int PlanetCount = 8;

    [Property(Tooltip = "Fewest moons a planet can get.")]
    public int MinMoons = 0;

    [Property(Tooltip = "Most moons a planet can get. Total body count is roughly PlanetCount * (1 + this/2).")]
    public int MaxMoons = 3;

    [Property(Tooltip = "Seed for the layout. Same seed rebuilds the same system.")]
    public int Seed = 1337;

    [Property(Tooltip = "Radius of the central sun, in world units.")]
    public float SunRadius = 260.0f;

    [Property(Tooltip = "Orbit radius of the innermost planet.")]
    public float FirstOrbitRadius = 900.0f;

    [Property(Tooltip = "Extra orbit radius added per planet as you go outward.")]
    public float OrbitSpacing = 620.0f;

    [Property(Tooltip = "Multiplies every orbit and spin rate. 0 freezes the system, negative runs it backwards.")]
    public float TimeScale = 1.0f;

    [Property(Tooltip = "Longitude segments per sphere. Vertex count per body is roughly this times half of it.")]
    public int SphereSegments = 32;

    [Property(Tooltip = "Put a point light inside the sun so the planets are actually lit by it.")]
    public bool SunLight = true;

    [Property(Tooltip = "Reach of the sun's light. Bodies past this fall into shadow.")]
    public float SunLightRange = 20000.0f;

    [Property(Tooltip = "Log the body and vertex totals once the system is built.")]
    public bool LogStats = true;

    // One entry per orbiting body. Moons carry the index of the planet they belong to; the sun is not in
    // here at all because it never moves.
    private struct Body
    {
        public Entity Entity;
        public int    ParentPlanet;    // -1 for a planet, else index into Planets
        public float  OrbitRadius;
        public float  OrbitSpeed;      // radians/sec
        public float  OrbitPhase;      // radians at t = 0
        public float  Inclination;     // radians the orbit plane tilts off horizontal
        public float  SpinSpeed;       // radians/sec about the body's own axis
        public FVector3 SpinAxis;
    }

    private readonly List<Body> Planets = new();
    private readonly List<Body> Moons   = new();

    // World position each planet resolved to this frame, so the moon pass can orbit around it without
    // recomputing the planet's orbit or reading the transform back out of the registry.
    private FVector3[] PlanetPositions = Array.Empty<FVector3>();

    private Entity   SunEntity = Entity.Null;
    private FVector3 SunOrigin;
    private float    Time;

    // Sphere build scratch, sized once for the largest body and reused for every Commit.
    private FVector3[] Positions = Array.Empty<FVector3>();
    private FVector3[] Normals   = Array.Empty<FVector3>();
    private FVector2[] UVs       = Array.Empty<FVector2>();
    private FVector4[] Colors    = Array.Empty<FVector4>();
    private int[]      Indices   = Array.Empty<int>();
    private int        ScratchSegments = -1;

    public override void OnReady()
    {
        int Segments = Math.Clamp(SphereSegments, 6, 128);
        int Rings    = Math.Max(3, Segments / 2);
        EnsureScratch(Segments, Rings);

        // Everything is laid out relative to wherever this entity sits, so dropping the script on an
        // entity somewhere out in the level builds the system there rather than always at the origin.
        SunOrigin = World.GetEntityLocation(Entity);

        Random Rng = new(Seed);

        BuildSun(Segments, Rings);

        int Count = Math.Clamp(PlanetCount, 0, 512);
        for (int i = 0; i < Count; ++i)
        {
            BuildPlanet(i, Rng, Segments, Rings);
        }

        PlanetPositions = new FVector3[Planets.Count];

        if (LogStats)
        {
            int PerBody = Segments * Rings;
            int Bodies  = 1 + Planets.Count + Moons.Count;
            Debug.Log($"UniverseGenerator: {Bodies} bodies ({Planets.Count} planets, {Moons.Count} moons), " +
                      $"~{PerBody} verts each, ~{Bodies * PerBody} total.");
        }

        // Put the bodies where they belong before the first frame is drawn, otherwise everything renders
        // stacked at the sun for one frame.
        Simulate();
    }

    public override void OnUpdate(float DeltaTime)
    {
        Time += DeltaTime * TimeScale;
        Simulate();
    }

    /// <summary>Destroy everything we spawned so stopping and replaying doesn't stack a second system.</summary>
    public override void OnDetach()
    {
        foreach (Body Planet in Planets)
        {
            World.DestroyEntity(Planet.Entity);
        }
        foreach (Body Moon in Moons)
        {
            World.DestroyEntity(Moon.Entity);
        }
        if (!SunEntity.IsNull)
        {
            World.DestroyEntity(SunEntity);
        }

        Planets.Clear();
        Moons.Clear();
        PlanetPositions = Array.Empty<FVector3>();
        SunEntity       = Entity.Null;
    }

    // ---------------------------------------------------------------------------------------------
    // Simulation
    // ---------------------------------------------------------------------------------------------

    /// <summary>
    /// Advances every body to its position for the current Time. Planets resolve first and cache their
    /// world position, so the moon pass is a straight offset from an already-known point.
    /// </summary>
    private void Simulate()
    {
        for (int i = 0; i < Planets.Count; ++i)
        {
            Body Planet = Planets[i];

            FVector3 Position = SunOrigin + OrbitOffset(Planet, Time);
            PlanetPositions[i] = Position;

            World.SetEntityLocation(Planet.Entity, Position);
            World.SetEntityRotation(Planet.Entity, Spin(Planet, Time));
        }

        foreach (Body Moon in Moons)
        {
            // ParentPlanet is always a valid index: moons are only ever created inside BuildPlanet.
            FVector3 Position = PlanetPositions[Moon.ParentPlanet] + OrbitOffset(Moon, Time);

            World.SetEntityLocation(Moon.Entity, Position);
            World.SetEntityRotation(Moon.Entity, Spin(Moon, Time));
        }
    }

    /// <summary>Position on a circular orbit, tilted by the body's inclination about the X axis.</summary>
    private static FVector3 OrbitOffset(in Body B, float T)
    {
        float Angle = B.OrbitPhase + B.OrbitSpeed * T;

        float X = MathF.Cos(Angle) * B.OrbitRadius;
        float Z = MathF.Sin(Angle) * B.OrbitRadius;

        // Tilt the flat orbit out of the XZ plane so the system reads as three-dimensional instead of
        // everything sitting on one disc.
        float SinI = MathF.Sin(B.Inclination);
        float CosI = MathF.Cos(B.Inclination);

        return new FVector3(X, Z * SinI, Z * CosI);
    }

    private static FQuat Spin(in Body B, float T) => FQuat.FromAxisAngle(B.SpinAxis, B.SpinSpeed * T);

    // ---------------------------------------------------------------------------------------------
    // Construction
    // ---------------------------------------------------------------------------------------------

    private void BuildSun(int Segments, int Rings)
    {
        SunEntity = World.CreateEntity("Sun", SunOrigin);
        if (SunEntity.IsNull)
        {
            Debug.LogError("UniverseGenerator: could not create the sun entity.");
            return;
        }

        // Near-white core with a warm limb, so the sphere still reads as a sphere even before lighting.
        CommitSphere(SunEntity, MathF.Max(SunRadius, 1.0f), Segments, Rings,
                     new FVector4(1.00f, 0.93f, 0.65f, 1.0f),
                     new FVector4(1.00f, 0.55f, 0.12f, 1.0f),
                     Bands: 0.0f);

        if (!SunLight)
        {
            return;
        }

        SPointLightComponent? Light = World.Emplace<SPointLightComponent>(SunEntity);
        if (Light == null)
        {
            Debug.LogWarning("UniverseGenerator: sun created without a point light (Emplace failed).");
            return;
        }

        Light.LightColor  = new FVector3(1.0f, 0.86f, 0.62f);
        Light.Intensity   = 250.0f;
        Light.Attenuation = MathF.Max(SunLightRange, 1.0f);
        Light.Falloff     = 0.35f;
    }

    private void BuildPlanet(int Index, Random Rng, int Segments, int Rings)
    {
        float OrbitRadius = FirstOrbitRadius + OrbitSpacing * Index;

        // Outer planets orbit slower (loosely Keplerian -- it only has to look right, not be right).
        float OrbitSpeed = 0.55f / MathF.Sqrt(OrbitRadius / MathF.Max(FirstOrbitRadius, 1.0f));
        float Radius     = Lerp(48.0f, 130.0f, (float)Rng.NextDouble());

        Entity PlanetEntity = World.CreateEntity($"Planet_{Index}", SunOrigin);
        if (PlanetEntity.IsNull)
        {
            return;
        }

        FVector4 Tint = PlanetTint(Rng);
        CommitSphere(PlanetEntity, Radius, Segments, Rings,
                     Tint, Tint * 0.55f, Bands: (float)Rng.Next(3, 9));

        Body Planet = new()
        {
            Entity       = PlanetEntity,
            ParentPlanet = -1,
            OrbitRadius  = OrbitRadius,
            OrbitSpeed   = OrbitSpeed,
            OrbitPhase   = (float)(Rng.NextDouble() * Math.PI * 2.0),
            Inclination  = (float)((Rng.NextDouble() - 0.5) * 0.35),
            SpinSpeed    = Lerp(0.4f, 1.8f, (float)Rng.NextDouble()),
            SpinAxis     = RandomAxis(Rng),
        };
        Planets.Add(Planet);

        int PlanetIndex = Planets.Count - 1;

        int Low   = Math.Max(0, MinMoons);
        int High  = Math.Max(Low, MaxMoons);
        int Count = Rng.Next(Low, High + 1);

        for (int m = 0; m < Count; ++m)
        {
            BuildMoon(PlanetIndex, Index, m, Radius, Rng, Segments, Rings);
        }
    }

    private void BuildMoon(int PlanetIndex, int PlanetLabel, int MoonLabel, float PlanetRadius,
                           Random Rng, int Segments, int Rings)
    {
        Entity MoonEntity = World.CreateEntity($"Planet_{PlanetLabel}_Moon_{MoonLabel}", SunOrigin);
        if (MoonEntity.IsNull)
        {
            return;
        }

        float Radius = PlanetRadius * Lerp(0.18f, 0.34f, (float)Rng.NextDouble());

        // Start clear of the planet's surface and step outward per moon so they never intersect.
        float OrbitRadius = PlanetRadius * 2.4f + MoonLabel * PlanetRadius * 1.15f;

        FVector4 Grey = new(0.62f, 0.62f, 0.66f, 1.0f);
        CommitSphere(MoonEntity, Radius, Segments, Rings, Grey, Grey * 0.45f, Bands: 0.0f);

        Moons.Add(new Body
        {
            Entity       = MoonEntity,
            ParentPlanet = PlanetIndex,
            OrbitRadius  = OrbitRadius,
            OrbitSpeed   = Lerp(1.6f, 3.4f, (float)Rng.NextDouble()),
            OrbitPhase   = (float)(Rng.NextDouble() * Math.PI * 2.0),
            Inclination  = (float)((Rng.NextDouble() - 0.5) * 1.1),
            SpinSpeed    = Lerp(0.3f, 1.2f, (float)Rng.NextDouble()),
            SpinAxis     = RandomAxis(Rng),
        });
    }

    // ---------------------------------------------------------------------------------------------
    // Mesh
    // ---------------------------------------------------------------------------------------------

    /// <summary>
    /// Adds a dynamic mesh component to the entity and commits a UV sphere of the given radius into it.
    /// Normals are exact (a unit sphere's normal IS its position), so the derive-normals path is skipped.
    /// </summary>
    private void CommitSphere(Entity Target, float Radius, int Segments, int Rings,
                              FVector4 ColorA, FVector4 ColorB, float Bands)
    {
        SDynamicMeshComponent? Mesh = World.Emplace<SDynamicMeshComponent>(Target);
        if (Mesh == null)
        {
            Debug.LogError("UniverseGenerator: could not add a dynamic mesh component.");
            return;
        }

        int Vertex = 0;

        // Rings run pole to pole; the seam column is duplicated (u = 0 and u = 1) so the UVs don't wrap
        // backwards across it.
        for (int Ring = 0; Ring <= Rings; ++Ring)
        {
            float v     = (float)Ring / Rings;
            float Phi   = v * MathF.PI;              // 0 at +Y pole, PI at -Y pole
            float SinP  = MathF.Sin(Phi);
            float CosP  = MathF.Cos(Phi);

            // Latitude banding, so gas giants get stripes and everything else gets a soft pole-to-equator
            // gradient. Bands = 0 collapses to the plain gradient.
            float Band = Bands > 0.0f ? 0.5f + 0.5f * MathF.Cos(v * MathF.PI * 2.0f * Bands) : v;
            FVector4 Tint = LerpColor(ColorA, ColorB, Band);

            for (int Seg = 0; Seg <= Segments; ++Seg)
            {
                float u     = (float)Seg / Segments;
                float Theta = u * MathF.PI * 2.0f;

                FVector3 Normal = new(SinP * MathF.Cos(Theta), CosP, SinP * MathF.Sin(Theta));

                Positions[Vertex] = Normal * Radius;
                Normals[Vertex]   = Normal;
                UVs[Vertex]       = new FVector2(u, v);
                Colors[Vertex]    = Tint;
                ++Vertex;
            }
        }

        int Stride = Segments + 1;
        int Tri    = 0;

        for (int Ring = 0; Ring < Rings; ++Ring)
        {
            for (int Seg = 0; Seg < Segments; ++Seg)
            {
                int A = Ring * Stride + Seg;
                int B = A + Stride;

                // Wound so cross(v1 - v0, v2 - v0) points OUTWARD, which is what the renderer treats as
                // front-facing (verified against the voxel mesher's +Y face). Reversed, every sphere is
                // backface-culled and the whole system renders invisible.
                //
                // Pole rings collapse to a single point, so one triangle of each pair is degenerate there.
                if (Ring != 0)
                {
                    Indices[Tri++] = A; Indices[Tri++] = A + 1; Indices[Tri++] = B;
                }
                if (Ring != Rings - 1)
                {
                    Indices[Tri++] = A + 1; Indices[Tri++] = B + 1; Indices[Tri++] = B;
                }
            }
        }

        // LOD 0 only. Neither this nor tangents earns its cost: nothing samples a normal map, and both
        // LOD building and MikkTSpace are the expensive stages of a Commit.
        Mesh.MaxLODs           = 1;
        Mesh.bGenerateTangents = false;

        Mesh.SetPositions(new ReadOnlySpan<FVector3>(Positions, 0, Vertex));
        Mesh.SetNormals(new ReadOnlySpan<FVector3>(Normals, 0, Vertex));
        Mesh.SetUVs(new ReadOnlySpan<FVector2>(UVs, 0, Vertex));
        Mesh.SetColors(new ReadOnlySpan<FVector4>(Colors, 0, Vertex));
        Mesh.SetIndices(new ReadOnlySpan<int>(Indices, 0, Tri));

        if (!Mesh.Commit())
        {
            Debug.LogError("UniverseGenerator: Commit() rejected a sphere.");
        }
    }

    private void EnsureScratch(int Segments, int Rings)
    {
        if (ScratchSegments == Segments)
        {
            return;
        }

        int VertexCount = (Rings + 1) * (Segments + 1);
        Positions = new FVector3[VertexCount];
        Normals   = new FVector3[VertexCount];
        UVs       = new FVector2[VertexCount];
        Colors    = new FVector4[VertexCount];
        Indices   = new int[Rings * Segments * 6];

        ScratchSegments = Segments;
    }

    // ---------------------------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------------------------

    private static float Lerp(float A, float B, float T) => A + (B - A) * T;

    private static FVector4 LerpColor(FVector4 A, FVector4 B, float T) => new(
        Lerp(A.X, B.X, T), Lerp(A.Y, B.Y, T), Lerp(A.Z, B.Z, T), 1.0f);

    /// <summary>A saturated but not neon planet color, biased away from pure grey.</summary>
    private static FVector4 PlanetTint(Random Rng)
    {
        float Hue = (float)Rng.NextDouble();
        float H   = Hue * 6.0f;
        float X   = 1.0f - MathF.Abs(H % 2.0f - 1.0f);

        FVector4 Pure = (int)H switch
        {
            0 => new FVector4(1, X, 0, 1),
            1 => new FVector4(X, 1, 0, 1),
            2 => new FVector4(0, 1, X, 1),
            3 => new FVector4(0, X, 1, 1),
            4 => new FVector4(X, 0, 1, 1),
            _ => new FVector4(1, 0, X, 1),
        };

        // Pull it toward a mid grey so planets read as rock and gas rather than as colour swatches.
        float Mix = 0.45f + 0.25f * (float)Rng.NextDouble();
        return LerpColor(new FVector4(0.55f, 0.5f, 0.45f, 1.0f), Pure, Mix);
    }

    private static FVector3 RandomAxis(Random Rng)
    {
        // Mostly upright with a wobble, so spin looks like planetary tilt rather than tumbling.
        FVector3 Axis = new((float)(Rng.NextDouble() - 0.5) * 0.6f,
                            1.0f,
                            (float)(Rng.NextDouble() - 0.5) * 0.6f);

        float Len = MathF.Sqrt(Axis.X * Axis.X + Axis.Y * Axis.Y + Axis.Z * Axis.Z);
        return Len > 1e-6f ? Axis * (1.0f / Len) : new FVector3(0, 1, 0);
    }
}
