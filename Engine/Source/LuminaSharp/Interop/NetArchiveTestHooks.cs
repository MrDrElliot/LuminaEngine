using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// Drives the C# FNetArchive against the native one, so a C++ test proves the C# packing reads back what it wrote.
internal static unsafe class NetArchiveTestHooks
{
    private static readonly FVector3 Position = new(1.5f, -2.25f, 1000.0f);

    private static int Pack(FNetArchive Archive, bool bAlive, int Ammo, float Charge, uint Score, string Name, FVector3 At, FEntity Target, out (bool, int, float, uint, string, FVector3, FEntity) Read)
    {
        bool  ReadAlive  = Archive.Serialize(bAlive);
        int   ReadAmmo   = Archive.SerializeRange(Ammo, 0, 200);
        float ReadCharge = Archive.SerializeQuantized(Charge, 0.0f, 1.0f, 8);
        uint  ReadScore  = Archive.SerializeVarUInt(Score);
        string ReadName  = Archive.Serialize(Name);
        FVector3 ReadAt  = Archive.Serialize(At);
        FEntity ReadTarget = Archive.Serialize(Target);
        Read = (ReadAlive, ReadAmmo, ReadCharge, ReadScore, ReadName, ReadAt, ReadTarget);
        return Archive.HasError ? 1 : 0;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_NetArchiveWrite(IntPtr Archive)
    {
        var Net = new FNetArchive(Archive);
        int Failures = Net.IsWriting ? 0 : 1;
        Failures |= Pack(Net, true, 137, 0.5f, 300u, "Kokiri", Position, new FEntity(42u), out _) << 1;
        return Failures;
    }

    // Bits are a wrong mode, an archive error, then each field in the order Pack writes it.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_NetArchiveRead(IntPtr Archive)
    {
        var Net = new FNetArchive(Archive);
        int Failures = Net.IsReading ? 0 : 1;
        Failures |= Pack(Net, false, 0, 0.0f, 0u, "", default, FEntity.Null, out var Read) << 1;
        Failures |= Read.Item1 ? 0 : 1 << 2;
        Failures |= Read.Item2 == 137 ? 0 : 1 << 3;
        Failures |= Math.Abs(Read.Item3 - 0.5f) <= 1.0f / 255.0f ? 0 : 1 << 4;
        Failures |= Read.Item4 == 300u ? 0 : 1 << 5;
        Failures |= Read.Item5 == "Kokiri" ? 0 : 1 << 6;
        Failures |= Read.Item6.Equals(Position) ? 0 : 1 << 7;
        Failures |= Read.Item7.Id == 42u ? 0 : 1 << 8;
        return Failures;
    }

    // Bits are a struct with NetSerialize that is not found, one without it that is, and one of the wrong shape that is.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_DetectNetSerialize()
    {
        const string Source = """
            using Lumina;
            using LuminaSharp;
            [Reflect]
            public partial struct PackedProbe : INativeStruct<PackedProbe>
            {
                [Property] public int Ammo;
                public void NetSerialize(FNetArchive Archive) { Ammo = Archive.SerializeRange(Ammo, 0, 200); }
            }
            [Reflect]
            public partial struct PlainProbe : INativeStruct<PlainProbe>
            {
                [Property] public int Ammo;
            }
            [Reflect]
            public partial struct MisdeclaredProbe : INativeStruct<MisdeclaredProbe>
            {
                [Property] public int Ammo;
                public int NetSerialize(FNetArchive Archive) => Ammo;
            }
            """;
        FScriptImage? Image = ScriptCompiler.Compile("NetSerializeProbe", new[] { ("NetSerializeProbe.cs", Source) });
        if (Image is not { } Compiled)
        {
            return -1;
        }

        var Context = new ScriptLoadContext("NetSerializeProbe");
        try
        {
            var Library = new TypeLibrary(Context.LoadScriptAssembly("NetSerializeProbe", Compiled).GetTypes());
            int Failures = Library.FindNetSerializer("PackedProbe") != null ? 0 : 1;
            Failures |= Library.FindNetSerializer("PlainProbe") == null ? 0 : 2;
            Failures |= Library.FindNetSerializer("MisdeclaredProbe") == null ? 0 : 4;
            return Failures;
        }
        finally
        {
            Context.Unload();
        }
    }
}
