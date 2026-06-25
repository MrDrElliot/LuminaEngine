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

    [Property(Category = "Arrays")] public int[] IntArray;
    [Property(Category = "Arrays")] public List<float> FloatList;
    [Property(Category = "Arrays")] public string[] StringArray;
    [Property(Category = "Arrays")] public List<ETestMode> EnumList;
    [Property(Category = "Arrays")] public List<FVector3> VectorList;
    [Property(Category = "Arrays")] public TestStats[] StatsArray;
    [Property(Category = "Arrays")] public List<TestStats> StatsList;

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
}
