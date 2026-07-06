#nullable disable

using LuminaSharp;
using Lumina;
using System;
using System.Collections.Generic;

namespace Engine;

public enum ETestMode
{
    Idle,
    Running,
    Paused,
    Stopped,
}

public enum ETestPriority : byte
{
    None = 0,
    Low = 1,
    Medium = 2,
    High = 3,
}

/**
 * Just an engine script for testing different property types to validate visuals and serialization.
 */
public sealed class PropertyTestScript : EntityScript
{
    [Property(Category = "Numbers")] public sbyte I8Value;
    [Property(Category = "Numbers")] public byte U8Value;
    [Property(Category = "Numbers")] public short I16Value;
    [Property(Category = "Numbers")] public ushort U16Value;
    [Property(Category = "Numbers", Min = -100.0f, Max = 100.0f, Tooltip = "Clamped signed int.")] public int I32Value = 5;
    [Property(Category = "Numbers")] public uint U32Value;
    [Property(Category = "Numbers")] public long I64Value;
    [Property(Category = "Numbers")] public ulong U64Value;
    [Property(Category = "Numbers", Min = 0.0f, Max = 10.0f, Units = "m/s", Tooltip = "Clamped float with units.")] public float F32Value = 1.5f;
    [Property(Category = "Numbers")] public double F64Value = 3.14159;

    [Property(Category = "Basics", Tooltip = "A simple toggle.")] public bool BoolValue = true;
    [Property(Category = "Basics")] public string StringValue = "hello";

    [Property(Category = "Enums")] public ETestMode Mode = ETestMode.Running;
    [Property(Category = "Enums")] public ETestPriority Priority = ETestPriority.Medium;

    [Property(Category = "Vectors")] public FVector2 Vec2;
    [Property(Category = "Vectors")] public FVector3 Vec3 = new FVector3 { X = 1.0f, Y = 2.0f, Z = 3.0f };
    [Property(Category = "Vectors")] public FVector4 Vec4;
    [Property(Category = "Vectors")] public FQuat Rotation = new FQuat { X = 0.0f, Y = 0.0f, Z = 0.0f, W = 1.0f };
    [Property(Category = "Vectors", Color = true)] public FVector3 AmbientColor = new FVector3 { X = 1.0f, Y = 1.0f, Z = 1.0f };
    [Property(Category = "Vectors", Color = true)] public FVector4 TintColor = new FVector4 { X = 1.0f, Y = 1.0f, Z = 1.0f, W = 1.0f };
    [Property(Category = "Vectors")] public FIntVector2 IntVec2;
    [Property(Category = "Vectors")] public FIntVector3 IntVec3;
    [Property(Category = "Vectors")] public FUIntVector3 UIntVec3;
    [Property(Category = "Vectors")] public FTransform Xform;

    [Property(Category = "References", Tooltip = "Searchable entity dropdown.")] public Entity TargetEntity;
    [Property(Category = "References")] public FSoftObjectPath AssetPath;
    [Property(Category = "References")] public TSoftObjectPtr<CMaterial> Material;
    [Property(Category = "References")] public TSoftObjectPtr<CTexture> Texture;
    [Property(Category = "References")] public TObjectPtr<CStaticMesh> Mesh;

    [Property(Category = "Nested")] public TestStats Stats;
    [Property(Category = "Nested")] public TestContainer Container;

    // Instanced (polymorphic) objects. Pick a concrete type in the inspector and edit it inline. Opt-in
    // only; a field is instanced only when it carries [Instanced], whatever its declared type.
    [Property(Category = "Instanced", Tooltip = "Pick a command type and edit it inline."), Instanced] public ITestCommand Command;
    [Property(Category = "Instanced"), Instanced] public TestShapeBase Shape;

    // Lists of instanced objects. Each element independently picks a concrete type and edits inline.
    [Property(Category = "Instanced", Tooltip = "A list of commands, each independently typed."), Instanced] public List<ITestCommand> Commands;
    [Property(Category = "Instanced"), Instanced] public List<TestShapeBase> Shapes;

    [Property(Category = "Arrays")] public int[] IntArray;
    [Property(Category = "Arrays")] public List<float> FloatList;
    [Property(Category = "Arrays")] public string[] StringArray;
    [Property(Category = "Arrays")] public List<ETestMode> EnumList;
    [Property(Category = "Arrays")] public List<FVector3> VectorList;
    [Property(Category = "Arrays")] public TestStats[] StatsArray;
    [Property(Category = "Arrays")] public List<TestStats> StatsList;

    // Dictionary<K,V> maps to a reflected THashMap; keys edit inline, values render in the editor column.
    [Property(Category = "Maps")] public Dictionary<string, int> ScoresByName;
    [Property(Category = "Maps")] public Dictionary<int, string> NamesById;
    [Property(Category = "Maps")] public Dictionary<string, float> WeightsByName;
    [Property(Category = "Maps")] public Dictionary<string, bool> FlagsByName;
    [Property(Category = "Maps", Tooltip = "Unsigned integral key.")] public Dictionary<uint, int> CountsByU32;
    [Property(Category = "Maps", Tooltip = "64-bit integral key.")] public Dictionary<long, string> LabelsByI64;
    [Property(Category = "Maps")] public Dictionary<int, float> FloatByInt;
    [Property(Category = "Maps", Tooltip = "Enum key.")] public Dictionary<ETestMode, int> CountsByMode;
    [Property(Category = "Maps", Tooltip = "Enum value.")] public Dictionary<string, ETestPriority> PriorityByName;
    [Property(Category = "Maps", Tooltip = "Vector value.")] public Dictionary<string, FVector3> ColorsByName;
    [Property(Category = "Maps", Tooltip = "Entity-reference value.")] public Dictionary<string, Entity> EntitiesByName;
    [Property(Category = "Maps", Tooltip = "Soft asset-path value.")] public Dictionary<string, FSoftObjectPath> PathsByName;
    [Property(Category = "Maps", Tooltip = "Soft texture-pointer value.")] public Dictionary<string, TSoftObjectPtr<CTexture>> TexturesByName;

    // Struct value: each entry's value expands to an inline nested table (TestStats has [Property] members).
    [Property(Category = "Maps", Tooltip = "Struct value; expands to a nested table per entry.")] public Dictionary<string, TestStats> StatsByName;

    // Instanced value: each entry independently picks a concrete ITestCommand and edits it inline.
    [Property(Category = "Maps", Tooltip = "Instanced value; each entry picks a command type."), Instanced] public Dictionary<string, ITestCommand> CommandTable;

    [Button(Tooltip = "Reset the scalar fields to their defaults.")]
    public void ResetScalars()
    {
        I32Value = 5;
        F32Value = 1.5f;
        F64Value = 3.14159;
        BoolValue = true;
        StringValue = "hello";
    }

    [Button("Bump Vec3")]
    public void BumpVector()
    {
        Vec3.X += 1.0f;
        Vec3.Y += 1.0f;
        Vec3.Z += 1.0f;
    }

    [Button(Tooltip = "Fill a few maps from script to check external mutation reflects in the inspector.")]
    public void PopulateMaps()
    {
        ScoresByName = new Dictionary<string, int> { ["alpha"] = 1, ["beta"] = 2, ["gamma"] = 3 };
        CountsByMode = new Dictionary<ETestMode, int> { [ETestMode.Idle] = 0, [ETestMode.Running] = 5 };
        ColorsByName = new Dictionary<string, FVector3>
        {
            ["red"] = new FVector3 { X = 1.0f, Y = 0.0f, Z = 0.0f },
            ["green"] = new FVector3 { X = 0.0f, Y = 1.0f, Z = 0.0f },
            ["blue"] = new FVector3 { X = 0.0f, Y = 0.0f, Z = 1.0f },
        };
    }
}

public struct TestStats
{
    [Property] public int Health;
    [Property(Min = 0.0f, Max = 1.0f)] public float Armor;
    [Property] public string Label;
    [Property] public ETestMode Mode;
    [Property(Color = true)] public FVector3 Tint;
    [Property] public List<int> Scores;
}

public sealed class TestContainer
{
    [Property] public string Name;
    [Property] public FVector2 Offset;
    [Property] public TestStats Inner;
    [Property] public List<TestStats> Children;
    [Property] public Dictionary<string, int> Tags;
}

// An instanced-object family. A field typed as ITestCommand offers these concrete types in a picker
// and edits the chosen one's [Property] fields inline. Each candidate must be default-constructible.
public interface ITestCommand
{
}

public sealed class TestAttackCommand : ITestCommand
{
    [Property(Min = 0.0f)] public float Damage = 10.0f;
    [Property] public string Target = "Enemy";
    [Property] public ETestPriority Priority = ETestPriority.High;
}

public sealed class TestWaitCommand : ITestCommand
{
    [Property(Min = 0.0f, Units = "s")] public float Seconds = 1.0f;
    [Property] public bool Interruptible = true;
}

public sealed class TestLogCommand : ITestCommand
{
    [Property] public string Message = "Hello";
    [Property(Color = true)] public FVector3 Color = new FVector3 { X = 1.0f, Y = 1.0f, Z = 1.0f };
}

// A concrete base used as an instanced field via [Instanced]; the base itself is also selectable.
public class TestShapeBase
{
    [Property] public FVector3 Center;
}

public sealed class TestSphereShape : TestShapeBase
{
    [Property(Min = 0.0f)] public float Radius = 1.0f;
}

public sealed class TestBoxShape : TestShapeBase
{
    [Property] public FVector3 Extents = new FVector3 { X = 1.0f, Y = 1.0f, Z = 1.0f };
}
