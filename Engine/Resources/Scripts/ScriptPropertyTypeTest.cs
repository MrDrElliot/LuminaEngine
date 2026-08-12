using LuminaSharp;

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
public class ScriptPropertyTypeTest : EntityScript
{
    public enum ETestMode
    {
        Off,
        Slow,
        Fast,
    }

    //~ Blittable scalars: read and written in place at the property's offset, no boundary crossing.

    [Property(Category = "Scalars", Tooltip = "A 32-bit float.")]
    public float FloatValue = 3.5f;

    [Property(Category = "Scalars")]
    public double DoubleValue = -1.25;

    [Property(Category = "Scalars")]
    public bool BoolValue = true;

    [Property(Category = "Scalars")]
    public sbyte Int8Value = -3;

    [Property(Category = "Scalars")]
    public short Int16Value = -300;

    [Property(Category = "Scalars", Min = -100.0f, Max = 100.0f)]
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

    //~ An enum. The native slot is int64 wide whatever the C# underlying type is, so the accessor goes through
    //~ a long -- a 4-byte access would leave the top half of the slot stale on every write.

    [Property(Category = "Enum")]
    public ETestMode Mode = ETestMode.Slow;

    //~ Blittable struct mirrors: laid out identically on both sides, so they are read in place too. Their
    //~ initializers prove a default is not limited to what an attribute argument could have expressed.

    [Property(Category = "Structs")]
    public FVector2 Vector2Value = new FVector2(10.0f, 20.0f);

    [Property(Category = "Structs", Color = true)]
    public FVector3 Vector3Value = new FVector3(0.25f, 0.5f, 0.75f);

    [Property(Category = "Structs")]
    public FVector4 Vector4Value = new FVector4(1.0f, 0.0f, 0.0f, 1.0f);

    [Property(Category = "Structs")]
    public FTransform TransformValue = FTransform.Identity;

    //~ Storage-owning kinds: these need per-instance construction over the appended block. A memzeroed FString
    //~ reads as a plausible empty string and corrupts on the first assignment, which is what makes them the
    //~ interesting case rather than just another type.

    [Property(Category = "Text", Tooltip = "An FString living in native memory.")]
    public string StringValue = "declared default";

    [Property(Category = "References", Tooltip = "An asset reference, drawn as an asset picker.")]
    public FSoftObjectPath AssetValue = new FSoftObjectPath("/Engine/Resources/Content/DefaultMaterial");

    //~ Containers hand out a VIEW over the native container rather than a managed copy, because there is only
    //~ ever one copy of the value and native owns it. No initializer: assigning the view is meaningless.

    [Property(Category = "Containers")]
    public NativeList<int> IntList;

    [Property(Category = "Containers")]
    public NativeList<FVector3> VectorList;

    private int Failures;

    public override void OnReady()
    {
        Failures = 0;
        Debug.Log("[ScriptPropertyTypeTest] ---- begin ----");

        ReportAuthoredValues();

        // Each check restores what it found, so the sweep is non-destructive as a whole. Doing it per check
        // rather than with one snapshot up here is what keeps it honest: a field added to a check but missed
        // in a central snapshot would be silently clobbered, and the value it ate would be yours.
        CheckScalars();
        CheckEnum();
        CheckStructs();
        CheckString();
        CheckAsset();
        CheckContainers();

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
        Debug.Log($"[ScriptPropertyTypeTest]   Asset=\"{AssetValue.Path}\" (\"/Engine/Resources/Content/DefaultMaterial\")");
        Debug.Log($"[ScriptPropertyTypeTest]   IntList.Count={IntList.Count}  VectorList.Count={VectorList.Count} (containers have no default)");
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

        // Fast is the last member, so a truncated read or write of the int64 slot shows up here rather than
        // passing by accident on a zero value.
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

        (Vector2Value, Vector3Value, Vector4Value, TransformValue) = Saved;
    }

    private void CheckString()
    {
        string Saved = StringValue;

        // Long enough to need heap storage rather than fitting in the small-string buffer, which is the case
        // that actually proves the FString was constructed and not just memzeroed.
        const string Long = "a script string long enough to need heap storage rather than the small-string buffer";

        StringValue = "short";
        Check("string (short)", StringValue == "short", StringValue);

        StringValue = Long;
        Check("string (heap)", StringValue == Long, $"{StringValue.Length} chars");

        StringValue = "";
        Check("string (empty)", StringValue.Length == 0, "\"\"");

        StringValue = Saved;
    }

    private void CheckAsset()
    {
        FSoftObjectPath Saved = AssetValue;

        AssetValue = new FSoftObjectPath("/Engine/Resources/Content/SomeOtherAsset");
        Check("FSoftObjectPath", AssetValue.Path == "/Engine/Resources/Content/SomeOtherAsset", AssetValue.Path);

        AssetValue = new FSoftObjectPath("");
        Check("FSoftObjectPath (cleared)", !AssetValue.IsValid, "invalid");

        AssetValue = Saved;
    }

    private void CheckContainers()
    {
        // A view over the native container: mutating through it mutates the one copy of the value, so
        // re-reading the property has to see the same contents. Cleared at the end rather than restored --
        // a container has no authored default to put back, and leaving test data in it would look like state
        // the scene saved.
        NativeList<int> Ints = IntList;
        Ints.Clear();
        Ints.Add(10);
        Ints.Add(20);
        Ints.Add(30);

        Check("NativeList<int> count", IntList.Count == 3, IntList.Count.ToString());
        Check("NativeList<int> values", IntList.Count == 3 && IntList[0] == 10 && IntList[2] == 30,
            IntList.Count == 3 ? $"[{IntList[0]}, {IntList[1]}, {IntList[2]}]" : "<wrong count>");

        IntList.RemoveAt(0);
        Check("NativeList<int> remove", IntList.Count == 2 && IntList[0] == 20,
            IntList.Count == 2 ? $"[{IntList[0]}, {IntList[1]}]" : "<wrong count>");

        NativeList<FVector3> Vectors = VectorList;
        Vectors.Clear();
        Vectors.Add(new FVector3(1.0f, 0.0f, 0.0f));
        Vectors.Add(new FVector3(0.0f, 1.0f, 0.0f));

        Check("NativeList<FVector3>", VectorList.Count == 2 && VectorList[1].Y == 1.0f, VectorList.Count.ToString());

        IntList.Clear();
        VectorList.Clear();
    }
}
