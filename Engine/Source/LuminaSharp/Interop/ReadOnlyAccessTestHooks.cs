using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;

namespace LuminaSharp;

internal static unsafe class ReadOnlyAccessTestHooks
{
    private const string Probe = """
        using Lumina;
        using LuminaSharp;

        public struct ProbeScore : IResource
        {
            public int Points;
        }

        [Reflect]
        public partial struct ProbeHealth : IComponent<ProbeHealth>
        {
            [Property] public float Health;
        }

        public sealed class ProbeSystem : CEntitySystem
        {
            public TQuery<Read<STransformComponent>> Movers = new();
            public TQuery<Read<ProbeHealth>> Healths = new();
            public TRes<FWeatherState> Weather = new();
            public TRes<ProbeScore> Score = new();
            public TResMut<FWeatherState> WeatherMut = new();
            public TResMut<ProbeScore> ScoreMut = new();

            public void Run(Write<STransformComponent> Moved)
            {
                float Sum = 0.0f;
                Movers.ForEach((FEntity E, Read<STransformComponent> T) => Sum += T.Value.LocalTransform.Location.X + T.Value.GetLocalLocation().Y);
                Healths.ForEach((FEntity E, Read<ProbeHealth> H) => Sum += H.Value.Health);
                STransformComponent.ReadOnly Viewed = Moved.Value;
                Sum += Viewed.LocalTransform.Location.Z + Weather.Value.Wetness + Score.Value.Points;
                Moved.Value.LocalTransform.Location.X = Sum;
                WeatherMut.Value.Wetness = Sum;
                ScoreMut.Value.Points += 1;
                /*CASE*/
            }
        }
        """;

    private static readonly string[] Writes =
    {
        "Movers.ForEach((FEntity E, Read<STransformComponent> T) => T.Value.LocalTransform.Location.X = 1.0f);",
        "Movers.ForEach((FEntity E, Read<STransformComponent> T) => T.Value.SetLocalLocation(default));",
        "Weather.Value.Wetness = 1.0f;",
        "Score.Value.Points = 1;",
        "ref ProbeScore Held = ref Score.Value;",
        "Viewed.LocalTransform.Location.X = 1.0f;",
        "Healths.ForEach((FEntity E, Read<ProbeHealth> H) => H.Value.Health = 1.0f);",
    };

    // Case 0 is the reads and writes that must compile; every later case adds one write that must not.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ReadOnlyAccessErrors(int Case, byte* OutFirstError, int Capacity)
    {
        if (Case < 0 || Case > Writes.Length)
        {
            return -1;
        }
        string Source = Probe.Replace("/*CASE*/", Case == 0 ? string.Empty : Writes[Case - 1]);
        SyntaxTree Tree = CSharpSyntaxTree.ParseText(SourceText.From(Source, Encoding.UTF8), ScriptCompiler.ParseOptions, path: "ReadOnlyProbe.cs");
        MetadataReference[] References = ScriptCompiler.ReferencesWith(null, false);
        var RewriteErrors = new List<string>();
        Tree = ScriptPropertyRewriter.Rewrite(CSharpCompilation.Create("ReadOnlyProbe", new[] { Tree }, References, ScriptCompiler.CompilationOptions(false)), Tree, RewriteErrors);
        CSharpGeneratorDriver.Create(ScriptCompiler.SourceGenerators).RunGeneratorsAndUpdateCompilation(
            CSharpCompilation.Create("ReadOnlyProbe", new[] { Tree }, References, ScriptCompiler.CompilationOptions(false)), out Compilation Compiled, out _);
        string[] Errors = RewriteErrors.Concat(Compiled.GetDiagnostics().Where(Found => Found.Severity == DiagnosticSeverity.Error).Select(Found => Found.ToString())).ToArray();
        if (Errors.Length > 0 && Capacity > 0)
        {
            byte[] Text = Encoding.UTF8.GetBytes(Errors[0]);
            int Length = Math.Min(Text.Length, Capacity - 1);
            Text.AsSpan(0, Length).CopyTo(new Span<byte>(OutFirstError, Length));
            OutFirstError[Length] = 0;
        }
        return Errors.Length;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ReadOnlyAccessCaseCount() => Writes.Length + 1;
}
