using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// Drives the deterministic simulation from C#, so a native test can hold a C# peer to the state a C++ peer reaches.
internal static unsafe class SimulationTestHooks
{
    // The C# port of CSimTestSystem::OnExclusiveUpdate, line for line, so any difference in what the bindings return shows up as a desync.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_SimulationTick(IntPtr ContextAddress)
    {
        try
        {
            var Context = new SExclusiveContext(ContextAddress);
            SystemSimulation Sim = Context.Simulation;
            CWorld World = Context.World;

            if (Sim.Tick == 0)
            {
                for (uint Player = 0; Player < Sim.NumPlayers; ++Player)
                {
                    Context.Emplace<SSimTestBody>(Sim.Spawn()).Owner = (int)Player;
                }
            }

            foreach (FEntity Entity in Sim.GetEntities())
            {
                if (World.TryGetComponent<SSimTestBody>(Entity) is not { } Found)
                {
                    continue;
                }

                ref SSimTestBody Body = ref Found.Get();
                if (Body.Owner >= 0)
                {
                    SSimTestInput Input = Sim.GetInput<SSimTestInput>((uint)Body.Owner);
                    Body.X += Input.MoveX;
                    Body.Energy = Body.Energy * 0.9f + 0.37f * (float)Input.MoveX;
                    if (Input.bFire)
                    {
                        int FromX = Body.X;
                        ref SSimTestBody Shot = ref Context.Emplace<SSimTestBody>(Sim.Spawn());
                        Shot.X = FromX;
                        Shot.Energy = (float)(Sim.RandomUInt32() % 1000) * 0.001f;
                        Body = ref World.GetComponent<SSimTestBody>(Entity);
                    }
                }
                else
                {
                    Body.X += 3;
                    Body.Energy *= 1.01f;
                    if (Body.X > 120)
                    {
                        Sim.Destroy(Entity);
                        continue;
                    }
                }

                if (Sim.RandomUInt32() % 7 == 0)
                {
                    ++Body.Hits;
                }
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    // One if the input landed, zero without a session or when the session takes another struct.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_SimulationSubmit(IntPtr WorldAddress, int MoveX, int bFire)
    {
        try
        {
            CWorld World = NativeObjectMarshal.FromHandle<CWorld>(WorldAddress)!;
            return World.Simulation.SubmitInput(new SSimTestInput { MoveX = MoveX, bFire = bFire != 0 }) ? 1 : 0;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return -1;
        }
    }

    // Offers the session an input of the wrong type, which has to be refused rather than written.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_SimulationSubmitWrongType(IntPtr WorldAddress)
    {
        try
        {
            CWorld World = NativeObjectMarshal.FromHandle<CWorld>(WorldAddress)!;
            return World.Simulation.SubmitInput(new SCollisionEvent()) ? 1 : 0;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return -1;
        }
    }

    // Bit patterns of the C# SimMath, for a native sweep against the C++ one.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_SimMathEvaluate(float A, float B, float* Out)
    {
        Out[0] = SimMath.Sin(A);
        Out[1] = SimMath.Cos(A);
        Out[2] = SimMath.Atan(A);
        Out[3] = SimMath.Atan2(A, B);
        Out[4] = SimMath.WrapAngle(A);
    }
}
