using LuminaSharp;
using Lumina;

namespace Lumina.Tests;

/// <summary>
/// Exercises every property type the script layer supports, end to end, on a live entity.
///
/// NONE of these values live in C#. Each <c>[Property]</c> field is rewritten -- by ScriptPropertyRewriter,
/// inside the engine's own compile -- into a property over a real FProperty appended to this script's minted
/// CClass, past the C++ shim (Scripting::AppendScriptPropertiesToClass). So a pass here means the whole chain
/// works for that type: the C# schema reported it, the native planner laid it out, the value lifecycle brought
/// it up per instance, the accessor reads and writes the right bytes, and the property table and the tagged
/// serializer see it like any other reflected property.
///
/// The initializers below ARE the defaults. They are replayed once against the class default object, and every
/// new instance is copied from it -- the script equivalent of a C++ constructor seeding its CDO. A container
/// cannot have one (it is a view over storage native owns, so there is nothing to assign); fill it in OnReady.
///
/// Attach it to an entity and play. It logs, in order:
///   1. the AUTHORED values -- whatever the inspector, the scene or the defaults put there, read before this
///      script writes anything. A value you typed in and do not see here never reached the native object.
///   2. a round-trip check per type, which necessarily overwrites values -- so each check restores what it
///      found. Running this script must not disturb what you authored.
/// </summary>
[Reflect]
public class ScriptPropertyTypeTest : CEntityScript
{
    [Reflect]
    public enum ETestMode
    {
        Off,
        Slow,
        Fast,
    }

    //~ Blittable scalars: read and written in place at the property's offset, no boundary crossing.

    [Property(Category = "Scalars", ToolTip = "A 32-bit float.")]
    public float FloatValue = 3.5f;

    [Property(Category = "Scalars")]
    public double DoubleValue = -1.25;

    [Property(Category = "Scalars")]
    public bool BoolValue = true;

    [Property(Category = "Scalars")]
    public sbyte Int8Value = -3;

    [Property(Category = "Scalars")]
    public short Int16Value = -300;

    [Property(Category = "Scalars", ClampMin = -100.0f, ClampMax = 100.0f)]
    public int Int32Value = 7;

    [Property(Category = "Scalars")]
    public long Int64Value = 900000;

    [Property(Category = "Scalars")]
    public byte UInt8Value = 200;

    [Property(Category = "Scalars")]
    public ushort UInt16Value = 4000;

    [Property(Category = "Scalars")]
    public uint UInt32Value = 70000;

    [Property(Category = "Scalars")]
    public ulong UInt64Value = 12345678901;

    //~ The native enum slot is int64 wide, so the accessor goes through a long or the top half goes stale on write.

    [Property(Category = "Enum")]
    public ETestMode Mode = ETestMode.Slow;

    //~ Blittable struct mirrors are read in place, and their initializers prove a default is not limited to attribute arguments.

    [Property(Category = "Structs")]
    public FVector2 Vector2Value = new FVector2(10.0f, 20.0f);

    [Property(ColorPicker, Category = "Structs")]
    public FVector3 Vector3Value = new FVector3(0.25f, 0.5f, 0.75f);

    [Property(Category = "Structs")]
    public FVector4 Vector4Value = new FVector4(1.0f, 0.0f, 0.0f, 1.0f);

    [Property(Category = "Structs")]
    public FTransform TransformValue = FTransform.Identity;

    [Property(Category = "Structs")]
    public SDataTableRowHandle RowHandle;

    //~ Storage-owning kinds need per-instance construction, since a memzeroed FString reads as empty and corrupts on first assignment.

    [Property(Category = "Text", ToolTip = "An FString living in native memory.")]
    public string StringValue = "declared default";

    // An FName is an id and not text, so it is read and written in place and only ToString and FromString touch the name table.
    [Property(Category = "Text", ToolTip = "An interned FName.")]
    public FName NameValue = new FName("DeclaredName");

    [Property(Category = "References", ToolTip = "An asset reference, drawn as an asset picker.")]
    public FSoftObjectPath AssetValue = new FSoftObjectPath("/Engine/Resources/Content/DefaultMaterial");

    // Stored as the same FSoftObjectPath as the bare one, differing only in the picker filter, since references route by ISoftObjectReference.
    [Property(Category = "References", ToolTip = "A typed soft reference, drawn as a CTexture picker.")]
    public TSoftObjectPtr<CTexture> TextureRef = new TSoftObjectPtr<CTexture>("/Engine/Resources/Content/DefaultTexture");

    // A hard reference holds a live pointer that keeps its target alive, and null is the only default it can express.
    [Property(Category = "References", ToolTip = "A hard object reference, drawn as an object picker.")]
    public TStrongObjectPtr<CWorld> WorldRef;

    //~ Containers hand out a view over the native container since native owns the only copy, so assigning one is meaningless.

    [Property(Category = "Containers")]
    public TVector<int> IntList;

    [Property(Category = "Containers")]
    public TVector<FVector3> VectorList;

    [Property(Category = "Containers")]
    public THashMap<int, float> WeightByIndex;

    // FName is POD, so the blittable path carries it with a native stride of sizeof(FName).
    [Property(Category = "Containers")]
    public TVector<FName> Tags;

    // TVector<string> cannot exist because a managed reference cannot live in native memory.
    [Property(Category = "Containers")]
    public TVector<FString> Names;

    // A bare TStrongObjectPtr would be copied as raw bytes without refcounting, so writes here go through native assignment.
    [Property(Category = "Containers")]
    public TVector<TStrongObjectPtr<CWorld>> Worlds;

    // Each slot is a native FSoftObjectPath, so the view copies paths rather than bytes.
    [Property(Category = "Containers")]
    public TVector<TSoftObjectPtr<CTexture>> TextureList;

    private int Failures;

    public override void OnReady()
    {
        Failures = 0;
        Debug.Log("[ScriptPropertyTypeTest] ---- begin ----");

        ReportAuthoredValues();

        // Each check restores its own fields because a central snapshot could miss one and silently clobber your value.
        CheckScalars();
        CheckEnum();
        CheckStructs();
        CheckString();
        CheckName();
        CheckAsset();
        CheckObjectReference();
        CheckContainers();
        CheckObjectList();

        if (Failures == 0)
        {
            Debug.Log("[ScriptPropertyTypeTest] ---- all supported property types round-tripped ----");
        }
        else
        {
            Debug.LogError($"[ScriptPropertyTypeTest] ---- {Failures} property type(s) FAILED ----");
        }
    }

    /// <summary>
    /// What is in native storage before this script touches anything: the declared defaults on a fresh script,
    /// or whatever the inspector/scene authored over them.
    ///
    /// On a brand new instance every line here should read back the initializer at the top of this file. A
    /// zero instead means the default never reached the CDO; your edited value missing means the inspector
    /// write never reached the object.
    /// </summary>
    private void ReportAuthoredValues()
    {
        Debug.Log("[ScriptPropertyTypeTest] authored values as seen from C# (defaults in parentheses):");
        Debug.Log($"[ScriptPropertyTypeTest]   float={FloatValue} (3.5)  double={DoubleValue} (-1.25)  bool={BoolValue} (True)");
        Debug.Log($"[ScriptPropertyTypeTest]   sbyte={Int8Value} (-3)  short={Int16Value} (-300)  int={Int32Value} (7)  long={Int64Value} (900000)");
        Debug.Log($"[ScriptPropertyTypeTest]   byte={UInt8Value} (200)  ushort={UInt16Value} (4000)  uint={UInt32Value} (70000)  ulong={UInt64Value} (12345678901)");
        Debug.Log($"[ScriptPropertyTypeTest]   Mode={Mode} (Slow)");
        Debug.Log($"[ScriptPropertyTypeTest]   Vector2=({Vector2Value.X}, {Vector2Value.Y}) (10, 20)");
        Debug.Log($"[ScriptPropertyTypeTest]   Vector3=({Vector3Value.X}, {Vector3Value.Y}, {Vector3Value.Z}) (0.25, 0.5, 0.75)");
        Debug.Log($"[ScriptPropertyTypeTest]   Vector4=({Vector4Value.X}, {Vector4Value.Y}, {Vector4Value.Z}, {Vector4Value.W}) (1, 0, 0, 1)");
        Debug.Log($"[ScriptPropertyTypeTest]   Transform.Location=({TransformValue.Location.X}, {TransformValue.Location.Y}, {TransformValue.Location.Z}) (0, 0, 0)");
        Debug.Log($"[ScriptPropertyTypeTest]   String=\"{StringValue}\" (\"declared default\")");
        Debug.Log($"[ScriptPropertyTypeTest]   Name=\"{NameValue}\" (\"DeclaredName\")");
        Debug.Log($"[ScriptPropertyTypeTest]   Asset=\"{AssetValue.Path}\" (\"/Engine/Resources/Content/DefaultMaterial\")");
        Debug.Log($"[ScriptPropertyTypeTest]   TextureRef=\"{TextureRef.Path.Path}\" (\"/Engine/Resources/Content/DefaultTexture\")");
        Debug.Log($"[ScriptPropertyTypeTest]   WorldRef={(WorldRef.IsValid ? "set" : "null")} (null)");
        Debug.Log($"[ScriptPropertyTypeTest]   IntList.Count={IntList.Count}  VectorList.Count={VectorList.Count}  WeightByIndex.Count={WeightByIndex.Count}  Names.Count={Names.Count}  Tags.Count={Tags.Count}  Worlds.Count={Worlds.Count} (containers have no default)");
    }

    private void Check(string What, bool bCondition, string Detail)
    {
        if (bCondition)
        {
            Debug.Log($"[ScriptPropertyTypeTest]   ok   {What} = {Detail}");
        }
        else
        {
            Failures++;
            Debug.LogError($"[ScriptPropertyTypeTest]   FAIL {What} -> {Detail}");
        }
    }

    private void CheckScalars()
    {
        (float A, double B, bool C, sbyte D, short E, int F, long G, byte H, ushort I, uint J, ulong K) Saved =
            (FloatValue, DoubleValue, BoolValue, Int8Value, Int16Value, Int32Value, Int64Value,
             UInt8Value, UInt16Value, UInt32Value, UInt64Value);

        FloatValue = 1.25f;
        DoubleValue = -2.5;
        BoolValue = !Saved.C;
        Int8Value = -8;
        Int16Value = -16;
        Int32Value = -32;
        Int64Value = -64;
        UInt8Value = 8;
        UInt16Value = 16;
        UInt32Value = 32;
        UInt64Value = 64;

        Check("float",  FloatValue == 1.25f,   FloatValue.ToString());
        Check("double", DoubleValue == -2.5,   DoubleValue.ToString());
        Check("bool",   BoolValue == !Saved.C, BoolValue.ToString());
        Check("sbyte",  Int8Value == -8,       Int8Value.ToString());
        Check("short",  Int16Value == -16,     Int16Value.ToString());
        Check("int",    Int32Value == -32,     Int32Value.ToString());
        Check("long",   Int64Value == -64,     Int64Value.ToString());
        Check("byte",   UInt8Value == 8,       UInt8Value.ToString());
        Check("ushort", UInt16Value == 16,     UInt16Value.ToString());
        Check("uint",   UInt32Value == 32,     UInt32Value.ToString());
        Check("ulong",  UInt64Value == 64,     UInt64Value.ToString());

        (FloatValue, DoubleValue, BoolValue, Int8Value, Int16Value, Int32Value, Int64Value,
         UInt8Value, UInt16Value, UInt32Value, UInt64Value) = Saved;
    }

    private void CheckEnum()
    {
        ETestMode Saved = Mode;

        // Fast is the last member, so a truncated int64 slot access cannot pass by accident on zero.
        Mode = ETestMode.Fast;
        Check("enum", Mode == ETestMode.Fast, Mode.ToString());

        Mode = ETestMode.Off;
        Check("enum (reset)", Mode == ETestMode.Off, Mode.ToString());

        Mode = Saved;
    }

    private void CheckStructs()
    {
        (FVector2 A, FVector3 B, FVector4 C, FTransform D) Saved =
            (Vector2Value, Vector3Value, Vector4Value, TransformValue);

        Vector2Value = new FVector2(1.0f, 2.0f);
        Vector3Value = new FVector3(1.0f, 2.0f, 3.0f);
        Vector4Value = new FVector4(1.0f, 2.0f, 3.0f, 4.0f);

        FTransform Transform = FTransform.Identity;
        Transform.Location = new FVector3(5.0f, 6.0f, 7.0f);
        TransformValue = Transform;

        Check("FVector2", Vector2Value.X == 1.0f && Vector2Value.Y == 2.0f, $"({Vector2Value.X}, {Vector2Value.Y})");
        Check("FVector3", Vector3Value.X == 1.0f && Vector3Value.Z == 3.0f, $"({Vector3Value.X}, {Vector3Value.Y}, {Vector3Value.Z})");
        Check("FVector4", Vector4Value.W == 4.0f, $"({Vector4Value.X}, {Vector4Value.Y}, {Vector4Value.Z}, {Vector4Value.W})");
        Check("FTransform", TransformValue.Location.X == 5.0f && TransformValue.Location.Z == 7.0f,
            $"location ({TransformValue.Location.X}, {TransformValue.Location.Y}, {TransformValue.Location.Z})");

        string SavedRowName = RowHandle.RowName;
        RowHandle.RowName = "ScriptPropertyTypeTest";
        Check("SDataTableRowHandle", RowHandle.RowName == "ScriptPropertyTypeTest", RowHandle.RowName);
        RowHandle.RowName = SavedRowName;

        (Vector2Value, Vector3Value, Vector4Value, TransformValue) = Saved;
    }

    private void CheckString()
    {
        string Saved = StringValue;

        // Long enough to need heap storage, which proves the FString was constructed and not just memzeroed.
        const string Long = "a script string long enough to need heap storage rather than the small-string buffer";

        StringValue = "short";
        Check("string (short)", StringValue == "short", StringValue);

        StringValue = Long;
        Check("string (heap)", StringValue == Long, $"{StringValue.Length} chars");

        StringValue = "";
        Check("string (empty)", StringValue.Length == 0, "\"\"");

        StringValue = Saved;
    }

    private void CheckName()
    {
        FName Saved = NameValue;

        NameValue = new FName("Renamed");
        Check("FName", NameValue == new FName("Renamed"), NameValue.ToString());

        // Interning is case-insensitive, so this proves the id survived the round trip rather than the characters.
        Check("FName case-insensitive", NameValue == new FName("RENAMED"), "Renamed == RENAMED");

        // A value that kept its bytes but lost its interning would compare equal above and still fail here.
        Check("FName resolves to text", NameValue.ToString() == "Renamed", NameValue.ToString());

        NameValue = FName.None;
        Check("FName none", NameValue.IsNone && NameValue.ToString().Length == 0, "none");

        NameValue = Saved;
    }

    private void CheckAsset()
    {
        FSoftObjectPath Saved = AssetValue;

        AssetValue = new FSoftObjectPath("/Engine/Resources/Content/SomeOtherAsset");
        Check("FSoftObjectPath", AssetValue.Path == "/Engine/Resources/Content/SomeOtherAsset", AssetValue.Path);

        AssetValue = new FSoftObjectPath("");
        Check("FSoftObjectPath (cleared)", !AssetValue.IsValid, "invalid");

        AssetValue = Saved;

        // This proves the ISoftObjectReference routing rebuilds the wrapper type on read.
        TSoftObjectPtr<CTexture> SavedTexture = TextureRef;

        TextureRef = new TSoftObjectPtr<CTexture>("/Engine/Resources/Content/SomeTexture");
        Check("TSoftObjectPtr", TextureRef.Path.Path == "/Engine/Resources/Content/SomeTexture", TextureRef.Path.Path);

        TextureRef = new TSoftObjectPtr<CTexture>("");
        Check("TSoftObjectPtr (cleared)", !TextureRef.IsValid, "invalid");

        TextureRef = SavedTexture;
    }

    /// <summary>
    /// A hard object reference: a native object property holding a live CObject pointer, not a path.
    ///
    /// The world is the object used because it is the one CObject a script can always reach -- this file has
    /// to run in any project, with or without content. As everywhere else here the check restores what it
    /// found, so a hard reference to the world never survives into a save.
    /// </summary>
    private void CheckObjectReference()
    {
        TStrongObjectPtr<CWorld> Saved = WorldRef;
        CWorld Live = World;

        WorldRef = new TStrongObjectPtr<CWorld>(Live);
        Check("TStrongObjectPtr assign", WorldRef.IsValid, WorldRef.IsValid ? "valid" : "null");

        // Reads must return one canonical wrapper per native object or identity comparisons silently fail.
        Check("TStrongObjectPtr identity", ReferenceEquals(WorldRef.Value, Live), "same wrapper instance as World");
        Check("TStrongObjectPtr identity (re-read)", ReferenceEquals(WorldRef.Value, WorldRef.Value), "stable across reads");

        WorldRef = new TStrongObjectPtr<CWorld>(null);
        Check("TStrongObjectPtr cleared", !WorldRef.IsValid && WorldRef.Value is null, "null");

        WorldRef = Saved;
    }

    private void CheckContainers()
    {
        // Containers are cleared at the end rather than restored, since they have no authored default to put back.
        TVector<int> Ints = IntList;
        Ints.Clear();
        Ints.Add(10);
        Ints.Add(20);
        Ints.Add(30);

        Check("TVector<int> count", IntList.Count == 3, IntList.Count.ToString());
        Check("TVector<int> values", IntList.Count == 3 && IntList[0] == 10 && IntList[2] == 30,
            IntList.Count == 3 ? $"[{IntList[0]}, {IntList[1]}, {IntList[2]}]" : "<wrong count>");

        IntList.RemoveAt(0);
        Check("TVector<int> remove", IntList.Count == 2 && IntList[0] == 20,
            IntList.Count == 2 ? $"[{IntList[0]}, {IntList[1]}]" : "<wrong count>");

        TVector<FVector3> Vectors = VectorList;
        Vectors.Clear();
        Vectors.Add(new FVector3(1.0f, 0.0f, 0.0f));
        Vectors.Add(new FVector3(0.0f, 1.0f, 0.0f));

        Check("TVector<FVector3>", VectorList.Count == 2 && VectorList[1].Y == 1.0f, VectorList.Count.ToString());

        // An element by reference, which is what makes `List[i] = x` work through a property at all.
        IntList[0] = 99;
        Check("TVector<int> indexer set", IntList[0] == 99, IntList[0].ToString());

        THashMap<int, float> Weights = WeightByIndex;
        Weights.Clear();
        Weights.Set(1, 0.5f);
        Weights.Set(2, 1.5f);
        Weights.Set(1, 2.5f);   // insert-or-assign, so this overwrites rather than adding

        Check("THashMap count", WeightByIndex.Count == 2, WeightByIndex.Count.ToString());
        Check("THashMap overwrite", WeightByIndex.TryGetValue(1, out float One) && One == 2.5f, One.ToString());
        Check("THashMap contains", WeightByIndex.ContainsKey(2) && !WeightByIndex.ContainsKey(7), "1 and 2 present, 7 absent");

        float Sum = 0.0f;
        foreach (System.Collections.Generic.KeyValuePair<int, float> Pair in WeightByIndex)
        {
            Sum += Pair.Value;
        }
        Check("THashMap enumerate", Sum == 4.0f, Sum.ToString());

        WeightByIndex.Remove(2);
        Check("THashMap remove", WeightByIndex.Count == 1 && !WeightByIndex.ContainsKey(2), WeightByIndex.Count.ToString());

        // Get and Set are used over the indexer since a ref into the slot would let assignment copy the string's heap pointer.
        TVector<FString> Strings = Names;
        Strings.Clear();
        Strings.Add("first");
        // Long enough to need heap storage, which proves the element was constructed rather than memzeroed.
        Strings.Add("a name long enough to need heap storage rather than the small-string buffer");

        Check("TVector<FString> count", Names.Count == 2, Names.Count.ToString());
        Check("TVector<FString> read", Names.Get(0) == "first", Names.Get(0));
        Check("TVector<FString> heap", Names.Get(1).Length == 74, $"{Names.Get(1).Length} chars");

        Names.Set(0, "replaced");
        Check("TVector<FString> set", Names.Get(0) == "replaced", Names.Get(0));

        Names.Insert(1, "inserted");
        Check("TVector<FString> insert", Names.Count == 3 && Names.Get(1) == "inserted", Names.Get(1));
        Check("TVector<FString> indexof", Names.IndexOf("replaced") == 0, Names.IndexOf("replaced").ToString());

        Names.RemoveAt(0);
        Check("TVector<FString> remove", Names.Count == 2 && Names.Get(0) == "inserted", Names.Get(0));

        // The indexer refuses a marshalled element rather than hand out a reference an assignment could corrupt.
        bool bRefused = false;
        try { _ = Names[0]; } catch (System.NotSupportedException) { bRefused = true; }
        Check("TVector<FString> refuses ref", bRefused, "indexer throws, Get/Set is the way in");

        // FName elements ride the blittable path so the indexer works.
        TVector<FName> TagList = Tags;
        TagList.Clear();
        TagList.Add(new FName("Alpha"));
        TagList.Add(new FName("Beta"));

        Check("TVector<FName> count", Tags.Count == 2, Tags.Count.ToString());
        Check("TVector<FName> read", Tags[0] == new FName("Alpha"), Tags[0].ToString());
        Check("TVector<FName> indexof", Tags.IndexOf(new FName("Beta")) == 1, Tags.IndexOf(new FName("Beta")).ToString());

        Tags[0] = new FName("Gamma");
        Check("TVector<FName> indexer set", Tags[0] == new FName("Gamma"), Tags[0].ToString());

        TextureList.Clear();
        TextureList.Add(new TSoftObjectPtr<CTexture>("/Engine/Resources/Content/DefaultTexture"));
        TextureList.Add(default);
        Check("TVector<TSoftObjectPtr> read", TextureList.Count == 2 && TextureList.Get(0).Path.Path == "/Engine/Resources/Content/DefaultTexture",
            TextureList.Count.ToString());
        Check("TVector<TSoftObjectPtr> empty element", !TextureList.Get(1).IsValid, "invalid");
        TextureList.Set(1, new TSoftObjectPtr<CTexture>("/Engine/Resources/Content/SomeTexture"));
        Check("TVector<TSoftObjectPtr> set", TextureList.Get(1).Path.Path == "/Engine/Resources/Content/SomeTexture", TextureList.Get(1).Path.Path);

        IntList.Clear();
        VectorList.Clear();
        WeightByIndex.Clear();
        Names.Clear();
        Tags.Clear();
        TextureList.Clear();
    }

    /// <summary>
    /// The object-reference list, which is the container whose elements are not plain bytes.
    /// <para>Every write here goes through the native refcounted assignment rather than a byte copy, so what
    /// is actually under test is that Add/Set/Insert/RemoveAt each leave the reference counts right. A leak
    /// does not show up as a wrong value -- it shows up much later as an object that never dies -- so the
    /// observable half is checked here and the counting half is what the native assignment exists to do.</para>
    /// </summary>
    private void CheckObjectList()
    {
        CWorld Live = World;
        TStrongObjectPtr<CWorld> Ref = new TStrongObjectPtr<CWorld>(Live);

        TVector<TStrongObjectPtr<CWorld>> List = Worlds;
        List.Clear();
        List.Add(Ref);
        List.Add(default);

        Check("TVector<TStrongObjectPtr> count", Worlds.Count == 2, Worlds.Count.ToString());
        Check("TVector<TStrongObjectPtr> read", Worlds.Count == 2 && ReferenceEquals(Worlds.Get(0).Value, Live),
            "canonical wrapper");
        Check("TVector<TStrongObjectPtr> null element", Worlds.Count == 2 && !Worlds.Get(1).IsValid, "null");

        // Overwriting a slot must release the old object and retain the new one.
        Worlds.Set(1, Ref);
        Check("TVector<TStrongObjectPtr> set", ReferenceEquals(Worlds.Get(1).Value, Live), "assigned over null");

        Worlds.Set(0, default);
        Check("TVector<TStrongObjectPtr> clear slot", !Worlds.Get(0).IsValid, "null");

        Check("TVector<TStrongObjectPtr> indexof", Worlds.IndexOf(Ref) == 1, Worlds.IndexOf(Ref).ToString());

        Worlds.Insert(0, Ref);
        Check("TVector<TStrongObjectPtr> insert", Worlds.Count == 3 && ReferenceEquals(Worlds.Get(0).Value, Live),
            Worlds.Count.ToString());

        Worlds.RemoveAt(0);
        Check("TVector<TStrongObjectPtr> remove", Worlds.Count == 2, Worlds.Count.ToString());

        // Clearing releases every reference the list held, which is the half a leak would live in.
        Worlds.Clear();
        Check("TVector<TStrongObjectPtr> cleared", Worlds.Count == 0, Worlds.Count.ToString());
    }
}
