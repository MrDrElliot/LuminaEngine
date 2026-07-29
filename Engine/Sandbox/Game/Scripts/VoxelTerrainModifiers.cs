using System;
using LuminaSharp;

namespace Game;

/// <summary>Block ids. Index 0 is air; the rest double as material slot indices on the chunk mesh.</summary>
public static class Block
{
    public const byte Air    = 0;
    public const byte Grass  = 1;
    public const byte Dirt   = 2;
    public const byte Stone   = 3;
    public const byte Sand   = 4;
    public const byte Snow   = 5;
    public const byte Water  = 6;
    public const byte Wood   = 7;
    public const byte Leaves = 8;

    public const int Count = 9;
}

/// <summary>Per-column state the shaping stage builds up, one modifier after another.</summary>
public struct ColumnData
{
    /// <summary>Terrain height in blocks. Modifiers raise, carve and terrace this.</summary>
    public int Height;

    /// <summary>Block placed on the very top of the column.</summary>
    public byte Surface;

    /// <summary>Block filling the few layers under the surface.</summary>
    public byte Subsurface;

    /// <summary>Free 0..1 channel. Biome writes it, decorators read it, so tree cover can follow rainfall.</summary>
    public float Moisture;

    /// <summary>How steep the column is versus its neighbours, 0..1. Filled before the biome stage.</summary>
    public float Slope;
}

/// <summary>
/// Read-only world settings plus the shared deterministic noise, handed to every modifier. Everything a
/// modifier derives must come from world coordinates and the seed, never from chunk-local state -- chunks
/// are built independently and in any order, so anything else cracks at the seams.
/// </summary>
public sealed class TerrainContext
{
    public int Seed;
    public int MaxHeight;
    public int WaterLevel;

    /// <summary>Deterministic 0..1 hash of a world cell, salted so different consumers do not correlate.</summary>
    public float Hash(int X, int Z, int Salt)
    {
        uint H = (uint)(X * 374761393 + Z * 668265263 + Salt * 1274126177 + Seed * 2246822519);
        H = (H ^ (H >> 13)) * 1274126177u;
        return ((H ^ (H >> 16)) & 0xFFFFFFu) / (float)0xFFFFFF;
    }

    /// <summary>Value noise in world space, 0..1.</summary>
    public float Noise(float X, float Z, int Salt = 0)
    {
        int X0 = (int)MathF.Floor(X), Z0 = (int)MathF.Floor(Z);
        float Tx = X - X0, Tz = Z - Z0;
        Tx = Tx * Tx * (3.0f - 2.0f * Tx);
        Tz = Tz * Tz * (3.0f - 2.0f * Tz);

        float A = Hash(X0, Z0, Salt),     B = Hash(X0 + 1, Z0, Salt);
        float C = Hash(X0, Z0 + 1, Salt), D = Hash(X0 + 1, Z0 + 1, Salt);

        float AB = A + (B - A) * Tx;
        float CD = C + (D - C) * Tx;
        return AB + (CD - AB) * Tz;
    }

    /// <summary>Fractal value noise, 0..1.</summary>
    public float FBm(float X, float Z, int Octaves, int Salt = 0)
    {
        float Sum = 0.0f, Amp = 0.5f, Total = 0.0f;
        for (int i = 0; i < Octaves; ++i)
        {
            Sum += Noise(X, Z, Salt + i) * Amp;
            Total += Amp;
            X *= 2.0f; Z *= 2.0f; Amp *= 0.5f;
        }
        return Total > 0.0f ? Sum / Total : 0.0f;
    }

    /// <summary>Ridged noise, 0..1, peaking along thin lines. The shape rivers and mountain spines want.</summary>
    public float Ridge(float X, float Z, int Octaves, int Salt = 0)
    {
        float N = FBm(X, Z, Octaves, Salt);
        return 1.0f - MathF.Abs(N * 2.0f - 1.0f);
    }
}

/// <summary>A padded block volume for one chunk. Padding exists so decorations that straddle a chunk
/// border (a tree trunk one column out, its canopy reaching in) are placed identically by both chunks
/// and never clip at the seam. Writes outside the padded box are dropped.</summary>
public sealed class ChunkVolume
{
    public int Pad;
    public int SizeX, SizeY, SizeZ;   // padded dimensions
    public int OriginX, OriginZ;      // world column of local (0,0)

    private byte[] Blocks = Array.Empty<byte>();

    public void Resize(int ChunkSize, int Height, int Padding, int WorldX, int WorldZ)
    {
        Pad = Padding;
        SizeX = ChunkSize + Padding * 2;
        SizeZ = ChunkSize + Padding * 2;
        SizeY = Height;
        OriginX = WorldX - Padding;
        OriginZ = WorldZ - Padding;

        int Needed = SizeX * SizeY * SizeZ;
        if (Blocks.Length < Needed)
        {
            Blocks = new byte[Needed];
        }
        Array.Clear(Blocks, 0, Needed);
    }

    private int Index(int Lx, int Ly, int Lz) => (Ly * SizeZ + Lz) * SizeX + Lx;

    public bool InBounds(int Lx, int Ly, int Lz)
        => Lx >= 0 && Lx < SizeX && Ly >= 0 && Ly < SizeY && Lz >= 0 && Lz < SizeZ;

    public byte GetLocal(int Lx, int Ly, int Lz)
        => InBounds(Lx, Ly, Lz) ? Blocks[Index(Lx, Ly, Lz)] : Block.Air;

    public void SetLocal(int Lx, int Ly, int Lz, byte Value)
    {
        if (InBounds(Lx, Ly, Lz))
        {
            Blocks[Index(Lx, Ly, Lz)] = Value;
        }
    }

    /// <summary>World-column addressing; what decorators use so they never think in chunk-local space.</summary>
    public byte GetWorld(int Wx, int Y, int Wz) => GetLocal(Wx - OriginX, Y, Wz - OriginZ);

    public void SetWorld(int Wx, int Y, int Wz, byte Value) => SetLocal(Wx - OriginX, Y, Wz - OriginZ, Value);

    /// <summary>Fills a column solid up to Height, then stamps surface and subsurface.</summary>
    public void FillColumn(int Lx, int Lz, in ColumnData Column)
    {
        int Top = Math.Clamp(Column.Height, 0, SizeY - 1);
        for (int y = 0; y <= Top; ++y)
        {
            byte B = y == Top ? Column.Surface
                   : y >= Top - 3 ? Column.Subsurface
                   : Block.Stone;
            SetLocal(Lx, y, Lz, B);
        }
    }
}

/// <summary>
/// Base type for the terrain stack. Pick concrete modifiers in the inspector and reorder them -- they run
/// top to bottom, each one seeing what the previous ones produced.
///
/// Two stages. ShapeColumn runs first for every column and owns the heightfield and biome. Decorate runs
/// afterwards over the filled volume and can place any block anywhere, which is what trees, boulders and
/// caves need. A modifier implements whichever stage it cares about.
/// </summary>
public abstract class TerrainModifier
{
    [Property(Tooltip = "Skip this modifier without removing it from the stack.")]
    public bool Enabled = true;

    /// <summary>Heightfield / biome stage. Runs per column, in stack order.</summary>
    public virtual void ShapeColumn(TerrainContext Ctx, int WorldX, int WorldZ, ref ColumnData Column) { }

    /// <summary>Voxel stage, after every column is filled. Runs per column of the PADDED volume, so a
    /// decoration may reach into neighbouring chunks and still land identically in both.</summary>
    public virtual void Decorate(TerrainContext Ctx, ChunkVolume Volume, int WorldX, int WorldZ, in ColumnData Column) { }
}

// ---------------------------------------------------------------------------------------------------
// Shaping modifiers
// ---------------------------------------------------------------------------------------------------

/// <summary>Continent-scale rolling terrain. Put this first -- it establishes the height everything else edits.</summary>
public sealed class BaseTerrainModifier : TerrainModifier
{
    [Property(Tooltip = "Horizontal feature size. Smaller is broader, smoother land.")]
    public float Scale = 0.008f;

    [Property(Tooltip = "Detail octaves. More is rougher.")]
    public int Octaves = 4;

    [Property(Tooltip = "Fraction of MaxHeight the terrain can reach.")]
    public float Amplitude = 0.75f;

    [Property(Tooltip = "Lowest terrain height, in blocks.")]
    public int FloorHeight = 4;

    public override void ShapeColumn(TerrainContext Ctx, int WorldX, int WorldZ, ref ColumnData Column)
    {
        float Continent = Ctx.FBm(WorldX * Scale * 0.35f, WorldZ * Scale * 0.35f, 3, 11);
        float Detail    = Ctx.FBm(WorldX * Scale, WorldZ * Scale, Math.Clamp(Octaves, 1, 8), 23);

        float T = Continent * 0.65f + Detail * 0.35f;
        T = T * T * (3.0f - 2.0f * T);   // flatten the plains, steepen the slopes

        Column.Height = FloorHeight + (int)(T * Ctx.MaxHeight * Amplitude);
    }
}

/// <summary>Carves river valleys along ridged-noise lines. Run after the base terrain.</summary>
public sealed class ValleyModifier : TerrainModifier
{
    [Property(Tooltip = "How far apart the valleys run. Smaller is broader spacing.")]
    public float Scale = 0.004f;

    [Property(Tooltip = "Maximum carve depth, in blocks.")]
    public int Depth = 14;

    [Property(Tooltip = "Valley width, 0..1. Higher widens the cut.")]
    public float Width = 0.35f;

    [Property(Tooltip = "Flatten the valley floor toward sea level rather than just lowering it.")]
    public bool FlattenFloor = true;

    public override void ShapeColumn(TerrainContext Ctx, int WorldX, int WorldZ, ref ColumnData Column)
    {
        float R = Ctx.Ridge(WorldX * Scale, WorldZ * Scale, 3, 57);

        // Ridge peaks at 1 along the line; invert into a falloff centred on it.
        float Cut = MathF.Max(0.0f, R - (1.0f - Math.Clamp(Width, 0.05f, 0.95f)));
        if (Cut <= 0.0f)
        {
            return;
        }

        float Norm = Cut / Math.Clamp(Width, 0.05f, 0.95f);
        Norm = Norm * Norm * (3.0f - 2.0f * Norm);

        int Carved = Column.Height - (int)(Norm * Depth);
        if (FlattenFloor)
        {
            // Ease toward sea level so river beds sit flat instead of tracing the old terrain.
            Carved = (int)(Carved * (1.0f - Norm * 0.5f) + (Ctx.WaterLevel - 1) * (Norm * 0.5f));
        }
        Column.Height = Math.Max(1, Carved);
    }
}

/// <summary>Quantizes height into plateau steps -- the blocky mesa look.</summary>
public sealed class TerraceModifier : TerrainModifier
{
    [Property(Tooltip = "Blocks per step.")]
    public int StepHeight = 4;

    [Property(Tooltip = "Blend between smooth and fully stepped, 0..1.")]
    public float Strength = 0.7f;

    [Property(Tooltip = "Only terrace above this height, so shorelines stay natural.")]
    public int MinHeight = 16;

    public override void ShapeColumn(TerrainContext Ctx, int WorldX, int WorldZ, ref ColumnData Column)
    {
        if (Column.Height < MinHeight || StepHeight <= 1)
        {
            return;
        }

        int Stepped = Column.Height / StepHeight * StepHeight;
        float S = Math.Clamp(Strength, 0.0f, 1.0f);
        Column.Height = (int)(Column.Height * (1.0f - S) + Stepped * S);
    }
}

/// <summary>Chooses surface and subsurface blocks from height, slope and moisture. Run last of the
/// shaping modifiers, after everything that moves the height around.</summary>
public sealed class BiomeModifier : TerrainModifier
{
    [Property(Tooltip = "Snow above this height.")]
    public int SnowHeight = 42;

    [Property(Tooltip = "Bare rock above this height, or on steep ground.")]
    public int RockHeight = 34;

    [Property(Tooltip = "Slope above which rock shows through, 0..1.")]
    public float RockSlope = 0.55f;

    [Property(Tooltip = "Sand within this many blocks of sea level.")]
    public int ShoreBand = 2;

    [Property(Tooltip = "Moisture feature size; drives where forests can grow.")]
    public float MoistureScale = 0.003f;

    public override void ShapeColumn(TerrainContext Ctx, int WorldX, int WorldZ, ref ColumnData Column)
    {
        Column.Moisture = Ctx.FBm(WorldX * MoistureScale, WorldZ * MoistureScale, 2, 91);

        if (Column.Height <= Ctx.WaterLevel + ShoreBand)
        {
            Column.Surface = Block.Sand;
            Column.Subsurface = Block.Sand;
            return;
        }
        if (Column.Height >= SnowHeight)
        {
            Column.Surface = Block.Snow;
            Column.Subsurface = Block.Stone;
            return;
        }
        if (Column.Height >= RockHeight || Column.Slope >= RockSlope)
        {
            Column.Surface = Block.Stone;
            Column.Subsurface = Block.Stone;
            return;
        }

        Column.Surface = Block.Grass;
        Column.Subsurface = Block.Dirt;
    }
}

// ---------------------------------------------------------------------------------------------------
// Decoration modifiers
// ---------------------------------------------------------------------------------------------------

/// <summary>Scatters trees. Density follows the moisture channel the biome modifier wrote.</summary>
public sealed class TreeModifier : TerrainModifier
{
    [Property(Tooltip = "Chance per column, before moisture weighting.")]
    public float Density = 0.02f;

    [Property(Tooltip = "Only grow where moisture is at least this, 0..1.")]
    public float MinMoisture = 0.45f;

    [Property(Tooltip = "Shortest trunk, in blocks.")]
    public int MinTrunk = 4;

    [Property(Tooltip = "Tallest trunk, in blocks.")]
    public int MaxTrunk = 7;

    [Property(Tooltip = "Canopy radius, in blocks. Keep under the world's Padding or canopies clip at chunk seams.")]
    public int CanopyRadius = 2;

    [Property(Tooltip = "Lowest height trees will grow at. Keeps them off the beach.")]
    public int MinHeight = 14;

    public override void Decorate(TerrainContext Ctx, ChunkVolume Volume, int WorldX, int WorldZ, in ColumnData Column)
    {
        if (Column.Surface != Block.Grass || Column.Height < MinHeight || Column.Moisture < MinMoisture)
        {
            return;
        }

        // Moisture weights density so forests thicken rather than scattering evenly.
        float Weight = (Column.Moisture - MinMoisture) / MathF.Max(1e-3f, 1.0f - MinMoisture);
        if (Ctx.Hash(WorldX, WorldZ, 7717) > Density * Weight)
        {
            return;
        }

        int Trunk = MinTrunk + (int)(Ctx.Hash(WorldX, WorldZ, 991) * MathF.Max(1, MaxTrunk - MinTrunk + 1));
        int Base = Column.Height + 1;
        int Top  = Base + Trunk;

        for (int y = Base; y < Top; ++y)
        {
            Volume.SetWorld(WorldX, y, WorldZ, Block.Wood);
        }

        int R = Math.Max(1, CanopyRadius);
        for (int dy = -1; dy <= 1; ++dy)
        {
            int Radius = dy == 1 ? R - 1 : R;
            for (int dz = -Radius; dz <= Radius; ++dz)
            {
                for (int dx = -Radius; dx <= Radius; ++dx)
                {
                    // Round the corners off so the canopy reads as a blob, not a cube.
                    if (dx * dx + dz * dz > Radius * Radius + 1)
                    {
                        continue;
                    }
                    int Y = Top + dy;
                    if (Volume.GetWorld(WorldX + dx, Y, WorldZ + dz) == Block.Air)
                    {
                        Volume.SetWorld(WorldX + dx, Y, WorldZ + dz, Block.Leaves);
                    }
                }
            }
        }

        Volume.SetWorld(WorldX, Top + 1, WorldZ, Block.Leaves);
    }
}

/// <summary>Scatters stone boulders on open ground.</summary>
public sealed class BoulderModifier : TerrainModifier
{
    [Property(Tooltip = "Chance per column.")]
    public float Density = 0.006f;

    [Property(Tooltip = "Boulder radius, in blocks.")]
    public int Radius = 2;

    public override void Decorate(TerrainContext Ctx, ChunkVolume Volume, int WorldX, int WorldZ, in ColumnData Column)
    {
        if (Column.Height <= Ctx.WaterLevel || Ctx.Hash(WorldX, WorldZ, 4421) > Density)
        {
            return;
        }

        int R = Math.Max(1, Radius);
        int Cy = Column.Height + R - 1;
        for (int dy = -R; dy <= R; ++dy)
        {
            for (int dz = -R; dz <= R; ++dz)
            {
                for (int dx = -R; dx <= R; ++dx)
                {
                    if (dx * dx + dy * dy + dz * dz > R * R)
                    {
                        continue;
                    }
                    Volume.SetWorld(WorldX + dx, Cy + dy, WorldZ + dz, Block.Stone);
                }
            }
        }
    }
}

/// <summary>Carves 3D cave systems out of the filled volume. Shows a modifier that edits voxels
/// directly rather than the heightfield -- the mesher handles the resulting overhangs for free.</summary>
public sealed class CaveModifier : TerrainModifier
{
    [Property(Tooltip = "Cave feature size. Smaller is longer, wider tunnels.")]
    public float Scale = 0.05f;

    [Property(Tooltip = "How much of the rock gets carved, 0..1.")]
    public float Threshold = 0.58f;

    [Property(Tooltip = "Never carve above this height, so the surface keeps its shape.")]
    public int MaxHeight = 30;

    [Property(Tooltip = "Leave this many solid blocks under the surface as a roof.")]
    public int SurfaceRoof = 4;

    public override void Decorate(TerrainContext Ctx, ChunkVolume Volume, int WorldX, int WorldZ, in ColumnData Column)
    {
        int Top = Math.Min(MaxHeight, Column.Height - SurfaceRoof);
        for (int y = 1; y < Top; ++y)
        {
            // Two offset noise fields intersected: isolated blobs become connected tunnels.
            float A = Ctx.Noise(WorldX * Scale, WorldZ * Scale, 1200 + y * 7);
            float B = Ctx.Noise(WorldZ * Scale + 31.7f, WorldX * Scale + 11.3f, 3100 + y * 13);
            if (A > Threshold && B > Threshold)
            {
                Volume.SetWorld(WorldX, y, WorldZ, Block.Air);
            }
        }
    }
}
