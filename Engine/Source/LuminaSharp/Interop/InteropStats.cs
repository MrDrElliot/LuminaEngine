using System;
using System.Collections.Generic;
using System.Threading;

namespace LuminaSharp;

// Per-binding call counts for every generated native call, switched on by LUMINA_INTEROP_STATS=1 and reported to the log every few seconds.
public static class InteropStats
{
    private const int MaxBindings = 1 << 16;
    private const int ReportIntervalMs = 5000;
    private const int ReportedBindings = 40;

    // Read as a constant once the caller tiers up, so a disabled build pays nothing per call.
    public static readonly bool Enabled = Environment.GetEnvironmentVariable("LUMINA_INTEROP_STATS") == "1";

    public static readonly long[] Hits = new long[MaxBindings];

    private static readonly string[] Names = new string[MaxBindings];
    private static readonly long[] Reported = new long[MaxBindings];
    private static int Count;
    private static readonly Timer? Reporter = Enabled ? new Timer(_ => Report(), null, ReportIntervalMs, ReportIntervalMs) : null;

    // Slot 0 absorbs everything past the table, so a registration never fails a type initializer.
    public static int Register(string Name)
    {
        if (!Enabled)
        {
            return 0;
        }
        int Slot = Interlocked.Increment(ref Count);
        if (Slot >= MaxBindings)
        {
            return 0;
        }
        Names[Slot] = Name;
        return Slot;
    }

    private static void Report()
    {
        int Registered = Math.Min(Volatile.Read(ref Count), MaxBindings - 1);
        var Deltas = new List<(long Calls, string Name)>();
        long Total = 0;
        for (int Slot = 1; Slot <= Registered; ++Slot)
        {
            long Now = Volatile.Read(ref Hits[Slot]);
            long Delta = Now - Reported[Slot];
            Reported[Slot] = Now;
            if (Delta > 0 && Names[Slot] is string Name)
            {
                Deltas.Add((Delta, Name));
                Total += Delta;
            }
        }
        Deltas.Sort((A, B) => B.Calls.CompareTo(A.Calls));

        double Seconds = ReportIntervalMs / 1000.0;
        var Text = new System.Text.StringBuilder();
        Text.Append($"[InteropStats] {Total / Seconds:F0} native calls/s across {Deltas.Count} binding(s); top {Math.Min(ReportedBindings, Deltas.Count)}:");
        for (int i = 0; i < Deltas.Count && i < ReportedBindings; ++i)
        {
            Text.Append($"\n  {Deltas[i].Calls / Seconds,10:F0}/s  {Deltas[i].Name}");
        }
        Native.Log(ELogLevel.Info, Text.ToString());
    }
}
