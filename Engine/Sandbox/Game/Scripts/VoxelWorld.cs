using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Lumina;
using LuminaSharp;

namespace Game;

/// <summary>
/// Minecraft-style voxel terrain, streamed in chunks around this entity and built by a stack of terrain
/// modifiers you pick in the inspector.
///
/// Attach to any entity, press Play, then fill in Modifiers. They run top to bottom: a base terrain
/// modifier establishes the heightfield, valleys and terraces reshape it, a biome modifier chooses
/// surface blocks, and decorators (trees, boulders, caves) place voxels afterwards. Reorder them and the
/// terrain changes -- that ordering IS the generator.
///
/// Chunks mesh a real 3D block volume rather than a heightfield, so caves, overhangs and tree canopies
/// all work. Only faces touching air are emitted, so cost tracks surface area.
///
/// Generation is multi-threaded: a batch of chunks is shaped, decorated and meshed across the engine's
/// worker pool, then handed back to the game thread which is the only place entities may be created and
/// meshes committed. Chunks never look at each other, so the parallel half needs no synchronisation at all.
/// </summary>
public sealed class VoxelWorld : EntityScript
{
    [Property(Tooltip = "The generator. Runs in order; shaping modifiers first, then decorators.")]
    [Instanced]
    public List<TerrainModifier> Modifiers = new();

    [Property(Tooltip = "Columns per chunk edge. 16 matches Minecraft.")]
    public int ChunkSize = 16;

    [Property(Tooltip = "World units per block.")]
    public float VoxelSize = 100.0f;

    [Property(Tooltip = "Chunk radius kept loaded around this entity.")]
    public int ViewDistance = 6;

    [Property(Tooltip = "Chunks generated per frame. These are meshed in parallel across the worker pool, "
                      + "so this can be far higher than it could when generation was serial.")]
    public int ChunksPerFrame = 8;

    [Property(Tooltip = "World ceiling, in blocks.")]
    public int MaxHeight = 56;

    [Property(Tooltip = "Sea level, in blocks.")]
    public int WaterLevel = 12;

    [Property(Tooltip = "Terrain seed.")]
    public int Seed = 1337;

    [Property(Tooltip = "Columns of overlap built around each chunk so decorations crossing a chunk border "
                      + "land identically in both. Must exceed your largest canopy or boulder radius.")]
    public int Padding = 4;

    internal static readonly FVector4[] BlockColors =
    {
        new(0.00f, 0.00f, 0.00f, 0.0f),   // Air (never emitted)
        new(0.28f, 0.62f, 0.20f, 1.0f),   // Grass
        new(0.44f, 0.31f, 0.19f, 1.0f),   // Dirt
        new(0.42f, 0.42f, 0.45f, 1.0f),   // Stone
        new(0.83f, 0.76f, 0.53f, 1.0f),   // Sand
        new(0.95f, 0.96f, 0.99f, 1.0f),   // Snow
        new(0.16f, 0.36f, 0.62f, 0.8f),   // Water
        new(0.35f, 0.24f, 0.14f, 1.0f),   // Wood
        new(0.20f, 0.48f, 0.16f, 1.0f),   // Leaves
    };

    /// <summary>Immutable per-build inputs, copied once per batch so worker bodies never read a live
    /// [Property] field that the editor could be writing to underneath them.</summary>
    internal readonly struct BuildSettings
    {
        public readonly int ChunkSize, Height, WaterLevel, Pad;
        public readonly float VoxelSize;

        public BuildSettings(int ChunkSize, int Height, int WaterLevel, int Pad, float VoxelSize)
        {
            this.ChunkSize = ChunkSize;
            this.Height = Height;
            this.WaterLevel = WaterLevel;
            this.Pad = Pad;
            this.VoxelSize = VoxelSize;
        }
    }

    private readonly Dictionary<long, Entity> LoadedChunks = new();
    private readonly List<long> PendingRemoval = new();

    // One job per in-flight chunk. Each owns every buffer its generation touches, which is what makes the
    // parallel pass lock-free: no two jobs ever address the same memory.
    private readonly List<ChunkJob> Jobs = new();
    private readonly List<long> Batch = new();

    private readonly TerrainContext Ctx = new();

    private int CenterX, CenterZ;
    private bool bHasCenter;

    public override void OnReady()
    {
        if (Modifiers.Count == 0)
        {
            Debug.LogWarning("VoxelWorld: no modifiers assigned; the world will be empty. Add a "
                           + "BaseTerrainModifier (and usually a BiomeModifier) in the inspector.");
        }

        Debug.Log($"VoxelWorld: streaming on {Task.WorkerCount} worker threads.");
        StreamChunks(force: true);
    }

    public override void OnUpdate(float DeltaTime) => StreamChunks(force: false);

    public override void OnDetach()
    {
        foreach (var Pair in LoadedChunks)
        {
            World.DestroyEntity(Pair.Value);
        }
        LoadedChunks.Clear();
    }

    private void StreamChunks(bool force)
    {
        int Size = Math.Clamp(ChunkSize, 4, 64);
        int Radius = Math.Clamp(ViewDistance, 1, 32);
        float ChunkWorld = Size * VoxelSize;

        FVector3 Origin = Transform.GetLocation();
        int NewCenterX = (int)MathF.Floor(Origin.X / ChunkWorld);
        int NewCenterZ = (int)MathF.Floor(Origin.Z / ChunkWorld);

        // Only reconsider the working set when the player crosses a chunk boundary; in between, keep
        // draining whatever is still queued.
        if (force || !bHasCenter || NewCenterX != CenterX || NewCenterZ != CenterZ)
        {
            CenterX = NewCenterX;
            CenterZ = NewCenterZ;
            bHasCenter = true;

            PendingRemoval.Clear();
            foreach (var Pair in LoadedChunks)
            {
                Unpack(Pair.Key, out int cx, out int cz);
                if (Math.Abs(cx - CenterX) > Radius || Math.Abs(cz - CenterZ) > Radius)
                {
                    PendingRemoval.Add(Pair.Key);
                }
            }
            foreach (long Key in PendingRemoval)
            {
                World.DestroyEntity(LoadedChunks[Key]);
                LoadedChunks.Remove(Key);
            }
        }

        // Collect this frame's batch, nearest-first, so the hole under the player fills before the horizon.
        Batch.Clear();
        int Wanted = Math.Max(1, ChunksPerFrame);
        for (int Ring = 0; Ring <= Radius && Batch.Count < Wanted; ++Ring)
        {
            for (int dz = -Ring; dz <= Ring && Batch.Count < Wanted; ++dz)
            {
                for (int dx = -Ring; dx <= Ring && Batch.Count < Wanted; ++dx)
                {
                    if (Math.Max(Math.Abs(dx), Math.Abs(dz)) != Ring)
                    {
                        continue;   // ring shell only; the interior came from a smaller Ring
                    }

                    long Key = Pack(CenterX + dx, CenterZ + dz);
                    if (!LoadedChunks.ContainsKey(Key))
                    {
                        Batch.Add(Key);
                    }
                }
            }
        }

        if (Batch.Count == 0)
        {
            return;
        }

        Ctx.Seed = Seed;
        Ctx.MaxHeight = Math.Clamp(MaxHeight, 8, 255);
        Ctx.WaterLevel = WaterLevel;

        BuildSettings Settings = new(Size, Ctx.MaxHeight, WaterLevel, Math.Clamp(Padding, 0, 16), VoxelSize);

        while (Jobs.Count < Batch.Count)
        {
            Jobs.Add(new ChunkJob());
        }
        for (int i = 0; i < Batch.Count; ++i)
        {
            Unpack(Batch[i], out int cx, out int cz);
            Jobs[i].Prepare(cx, cz);
        }

        // The parallel half. Bodies are pure compute over per-job buffers and never touch the engine:
        // no entity, component or RHI call may happen off the game thread. Task.ParallelFor blocks, so
        // everything is complete and visible before the commit loop below runs.
        int Count = Batch.Count;
        Task.ParallelFor(Count, i => Jobs[i].Generate(Ctx, Modifiers, Settings));

        // The serial tail: entity creation and Commit are game-thread only.
        for (int i = 0; i < Count; ++i)
        {
            ChunkJob Job = Jobs[i];
            if (Job.Failure != null)
            {
                Debug.LogError($"VoxelWorld: chunk {Job.ChunkX},{Job.ChunkZ} failed to generate: {Job.Failure}");
                continue;
            }
            if (!Job.HasGeometry)
            {
                continue;   // fully empty chunk (all air); nothing to spawn
            }

            if (CommitChunk(Job, Settings, out Entity Spawned))
            {
                LoadedChunks[Pack(Job.ChunkX, Job.ChunkZ)] = Spawned;
            }
        }
    }

    /// <summary>Game thread only: turns a generated job into a live entity with a committed mesh.</summary>
    private bool CommitChunk(ChunkJob Job, in BuildSettings Settings, out Entity Spawned)
    {
        float ChunkWorld = Settings.ChunkSize * Settings.VoxelSize;
        Spawned = World.CreateEntity($"Chunk_{Job.ChunkX}_{Job.ChunkZ}",
                                     new FVector3(Job.ChunkX * ChunkWorld, 0.0f, Job.ChunkZ * ChunkWorld));
        if (Spawned.IsNull)
        {
            return false;
        }

        SDynamicMeshComponent? Mesh = World.Emplace<SDynamicMeshComponent>(Spawned);
        if (Mesh == null)
        {
            World.DestroyEntity(Spawned);
            Spawned = Entity.Null;
            return false;
        }

        // Rebuilt on stream-in and hard-edged, so neither extra LODs nor MikkTSpace tangents earn their cost.
        Mesh.MaxLODs = 1;
        Mesh.bGenerateTangents = false;

        Mesh.SetPositions(CollectionsMarshal.AsSpan(Job.Positions));
        Mesh.SetNormals(CollectionsMarshal.AsSpan(Job.Normals));   // supplied: derived normals would round the cube edges
        Mesh.SetUVs(CollectionsMarshal.AsSpan(Job.UVs));
        Mesh.SetColors(CollectionsMarshal.AsSpan(Job.Colors));
        Mesh.SetIndices(CollectionsMarshal.AsSpan(Job.MergedIndices));

        for (int Type = 0; Type < Block.Count; ++Type)
        {
            if (Job.SectionCount[Type] > 0)
            {
                Mesh.AddSection(Type, Job.SectionStart[Type], Job.SectionCount[Type]);
            }
        }

        Mesh.Commit();
        return true;
    }

    private static long Pack(int X, int Z) => ((long)(uint)X << 32) | (uint)Z;

    private static void Unpack(long Key, out int X, out int Z)
    {
        X = (int)(Key >> 32);
        Z = (int)(Key & 0xFFFFFFFF);
    }

    // ---------------------------------------------------------------------------------------------------
    // One chunk's generation. Everything here runs on a worker thread, so it owns all of its own storage
    // and reads only immutable inputs: the settings copy, the shared TerrainContext (pure math over the
    // seed), and the modifier list. A modifier that mutates its own fields during generation would break
    // that contract -- keep them stateless.
    // ---------------------------------------------------------------------------------------------------
    private sealed class ChunkJob
    {
        public int ChunkX, ChunkZ;
        public string? Failure;
        public bool HasGeometry;

        public readonly List<FVector3> Positions = new();
        public readonly List<FVector3> Normals   = new();
        public readonly List<FVector2> UVs       = new();
        public readonly List<FVector4> Colors    = new();
        public readonly List<int>      MergedIndices = new();
        public readonly int[] SectionStart = new int[Block.Count];
        public readonly int[] SectionCount = new int[Block.Count];

        private readonly ChunkVolume Volume = new();
        private readonly List<int>[] TypeIndices = new List<int>[Block.Count];
        private ColumnData[] Columns = Array.Empty<ColumnData>();
        private float VoxelSize = 1.0f;
        private int WaterLevel;

        public ChunkJob()
        {
            for (int i = 0; i < Block.Count; ++i)
            {
                TypeIndices[i] = new List<int>();
            }
        }

        public void Prepare(int X, int Z)
        {
            ChunkX = X;
            ChunkZ = Z;
            Failure = null;
            HasGeometry = false;
        }

        public void Generate(TerrainContext Ctx, List<TerrainModifier> Modifiers, in BuildSettings Settings)
        {
            try
            {
                Shape(Ctx, Modifiers, Settings);
                Mesh(Settings);
            }
            catch (Exception Ex)
            {
                // A worker fiber must never unwind into the native scheduler; capture and report on the
                // game thread instead.
                Failure = Ex.Message;
                HasGeometry = false;
            }
        }

        private void Shape(TerrainContext Ctx, List<TerrainModifier> Modifiers, in BuildSettings Settings)
        {
            int Size = Settings.ChunkSize, Pad = Settings.Pad, Height = Settings.Height;
            int BaseX = ChunkX * Size, BaseZ = ChunkZ * Size;

            VoxelSize = Settings.VoxelSize;
            WaterLevel = Settings.WaterLevel;

            Volume.Resize(Size, Height, Pad, BaseX, BaseZ);

            int Span = Size + Pad * 2;
            if (Columns.Length < Span * Span)
            {
                Columns = new ColumnData[Span * Span];
            }

            // Stage 1: shape every column of the padded region. Decorators need neighbouring columns shaped
            // too, or a tree just outside the chunk would have nothing to stand on.
            for (int lz = 0; lz < Span; ++lz)
            {
                for (int lx = 0; lx < Span; ++lx)
                {
                    ref ColumnData Column = ref Columns[lz * Span + lx];
                    Column = new ColumnData { Height = 1, Surface = Block.Stone, Subsurface = Block.Stone };

                    foreach (TerrainModifier Modifier in Modifiers)
                    {
                        if (Modifier is { Enabled: true })
                        {
                            Modifier.ShapeColumn(Ctx, BaseX - Pad + lx, BaseZ - Pad + lz, ref Column);
                        }
                    }

                    Column.Height = Math.Clamp(Column.Height, 1, Height - 1);
                }
            }

            // Slope from the shaped neighbours, so the biome stage can put rock on cliff faces. Done here
            // rather than inside a modifier because it needs columns that are already final.
            for (int lz = 1; lz < Span - 1; ++lz)
            {
                for (int lx = 1; lx < Span - 1; ++lx)
                {
                    int i = lz * Span + lx;
                    int Dx = Math.Abs(Columns[i + 1].Height - Columns[i - 1].Height);
                    int Dz = Math.Abs(Columns[i + Span].Height - Columns[i - Span].Height);
                    Columns[i].Slope = Math.Clamp(Math.Max(Dx, Dz) / 8.0f, 0.0f, 1.0f);
                }
            }

            // Re-run only the biome stage now that Slope exists, then fill the volume.
            for (int lz = 0; lz < Span; ++lz)
            {
                for (int lx = 0; lx < Span; ++lx)
                {
                    ref ColumnData Column = ref Columns[lz * Span + lx];
                    foreach (TerrainModifier Modifier in Modifiers)
                    {
                        if (Modifier is BiomeModifier { Enabled: true } Biome)
                        {
                            Biome.ShapeColumn(Ctx, BaseX - Pad + lx, BaseZ - Pad + lz, ref Column);
                        }
                    }
                    Volume.FillColumn(lx, lz, Column);
                }
            }

            // Stage 2: decorators write voxels, reaching across chunk borders into the padding.
            for (int lz = 0; lz < Span; ++lz)
            {
                for (int lx = 0; lx < Span; ++lx)
                {
                    ref ColumnData Column = ref Columns[lz * Span + lx];
                    foreach (TerrainModifier Modifier in Modifiers)
                    {
                        if (Modifier is { Enabled: true })
                        {
                            Modifier.Decorate(Ctx, Volume, BaseX - Pad + lx, BaseZ - Pad + lz, in Column);
                        }
                    }
                }
            }
        }

        private void Mesh(in BuildSettings Settings)
        {
            int Size = Settings.ChunkSize, Pad = Settings.Pad, Height = Settings.Height;

            Positions.Clear(); Normals.Clear(); UVs.Clear(); Colors.Clear();
            for (int i = 0; i < Block.Count; ++i)
            {
                TypeIndices[i].Clear();
            }

            // Only the chunk proper is meshed; the padding exists purely so border faces know their neighbour.
            for (int lz = Pad; lz < Pad + Size; ++lz)
            {
                for (int lx = Pad; lx < Pad + Size; ++lx)
                {
                    for (int y = 0; y < Height; ++y)
                    {
                        byte B = Volume.GetLocal(lx, y, lz);
                        if (B == Block.Air)
                        {
                            // One water surface quad at sea level wherever the column is open there.
                            if (y == WaterLevel && Volume.GetLocal(lx, y + 1, lz) == Block.Air)
                            {
                                EmitFace(lx - Pad, y, lz - Pad, 0, +1, 0, Block.Water, lx, y, lz);
                            }
                            continue;
                        }

                        // A face is visible exactly when its neighbour is air.
                        if (Volume.GetLocal(lx, y + 1, lz) == Block.Air)          EmitFace(lx - Pad, y, lz - Pad, 0, +1, 0, B, lx, y, lz);
                        if (y > 0 && Volume.GetLocal(lx, y - 1, lz) == Block.Air) EmitFace(lx - Pad, y, lz - Pad, 0, -1, 0, B, lx, y, lz);
                        if (Volume.GetLocal(lx + 1, y, lz) == Block.Air)          EmitFace(lx - Pad, y, lz - Pad, +1, 0, 0, B, lx, y, lz);
                        if (Volume.GetLocal(lx - 1, y, lz) == Block.Air)          EmitFace(lx - Pad, y, lz - Pad, -1, 0, 0, B, lx, y, lz);
                        if (Volume.GetLocal(lx, y, lz + 1) == Block.Air)          EmitFace(lx - Pad, y, lz - Pad, 0, 0, +1, B, lx, y, lz);
                        if (Volume.GetLocal(lx, y, lz - 1) == Block.Air)          EmitFace(lx - Pad, y, lz - Pad, 0, 0, -1, B, lx, y, lz);
                    }
                }
            }

            HasGeometry = Positions.Count > 0;
            if (!HasGeometry)
            {
                return;
            }

            // Concatenate the per-type runs so each becomes one contiguous section.
            MergedIndices.Clear();
            for (int Type = 0; Type < Block.Count; ++Type)
            {
                SectionStart[Type] = MergedIndices.Count;
                SectionCount[Type] = TypeIndices[Type].Count;
                MergedIndices.AddRange(TypeIndices[Type]);
            }
        }

        /// <summary>Emits one axis-aligned face, wound so (B-A)x(C-A) points along the normal.</summary>
        private void EmitFace(int Bx, int By, int Bz, int Nx, int Ny, int Nz, byte Type, int Vx, int Vy, int Vz)
        {
            float x0 = Bx * VoxelSize, y0 = By * VoxelSize, z0 = Bz * VoxelSize;
            float x1 = x0 + VoxelSize, y1 = y0 + VoxelSize, z1 = z0 + VoxelSize;

            FVector3 A, B, C, D;
            if (Ny > 0)       { A = new(x0, y1, z0); B = new(x0, y1, z1); C = new(x1, y1, z1); D = new(x1, y1, z0); }
            else if (Ny < 0)  { A = new(x0, y0, z1); B = new(x0, y0, z0); C = new(x1, y0, z0); D = new(x1, y0, z1); }
            else if (Nx > 0)  { A = new(x1, y0, z0); B = new(x1, y1, z0); C = new(x1, y1, z1); D = new(x1, y0, z1); }
            else if (Nx < 0)  { A = new(x0, y0, z1); B = new(x0, y1, z1); C = new(x0, y1, z0); D = new(x0, y0, z0); }
            else if (Nz > 0)  { A = new(x0, y0, z1); B = new(x1, y0, z1); C = new(x1, y1, z1); D = new(x0, y1, z1); }
            else              { A = new(x1, y0, z0); B = new(x0, y0, z0); C = new(x0, y1, z0); D = new(x1, y1, z0); }

            FVector3 N = new(Nx, Ny, Nz);

            // Minecraft's directional shading: flat tops full bright, undersides darkest, one horizontal axis
            // dimmer than the other. Costs nothing and does most of the work of making voxels read as solid.
            float Shade = Ny > 0 ? 1.00f : Ny < 0 ? 0.55f : Nx != 0 ? 0.72f : 0.86f;
            float Ao = FaceAO(Vx, Vy, Vz, Nx, Ny, Nz);

            int Base = Positions.Count;
            Positions.Add(A); Positions.Add(B); Positions.Add(C); Positions.Add(D);
            Normals.Add(N); Normals.Add(N); Normals.Add(N); Normals.Add(N);
            UVs.Add(new FVector2(0, 0)); UVs.Add(new FVector2(0, 1));
            UVs.Add(new FVector2(1, 1)); UVs.Add(new FVector2(1, 0));

            FVector4 Tint = BlockColors[Type];
            FVector4 Lit = new(Tint.X * Shade * Ao, Tint.Y * Shade * Ao, Tint.Z * Shade * Ao, Tint.W);
            Colors.Add(Lit); Colors.Add(Lit); Colors.Add(Lit); Colors.Add(Lit);

            List<int> Out = TypeIndices[Type];
            Out.Add(Base + 0); Out.Add(Base + 1); Out.Add(Base + 2);
            Out.Add(Base + 0); Out.Add(Base + 2); Out.Add(Base + 3);
        }

        /// <summary>Cheap per-face occlusion: how boxed-in the air cell in front of this face is.</summary>
        private float FaceAO(int Lx, int Ly, int Lz, int Nx, int Ny, int Nz)
        {
            int Fx = Lx + Nx, Fy = Ly + Ny, Fz = Lz + Nz;
            int Blocked = 0;
            if (Volume.GetLocal(Fx + 1, Fy, Fz) != Block.Air) ++Blocked;
            if (Volume.GetLocal(Fx - 1, Fy, Fz) != Block.Air) ++Blocked;
            if (Volume.GetLocal(Fx, Fy, Fz + 1) != Block.Air) ++Blocked;
            if (Volume.GetLocal(Fx, Fy, Fz - 1) != Block.Air) ++Blocked;
            return 1.0f - Blocked * 0.09f;
        }
    }
}
