using System;
using System.Runtime.CompilerServices;
using LuminaSharp;

namespace Lumina;

// The deterministic simulation as a Simulation stage system sees it, with the tick, every player's input, seeded draws and the entities it owns.
public readonly unsafe struct SystemSimulation
{
    private readonly SSystemContext Context;

    internal SystemSimulation(SSystemContext Context)
    {
        this.Context = Context;
    }

    public bool IsRunning => CSimulationLibrary.IsRunning(Context);

    // The tick being simulated, which is also the count of ticks before it.
    public uint Tick => CSimulationLibrary.GetTick(Context);

    public float TickSeconds => CSimulationLibrary.GetTickSeconds(Context);

    // A replay after a misprediction, where a sound or effect that already played should not play again.
    public bool IsResimulating => CSimulationLibrary.IsResimulating(Context);

    public uint NumPlayers => CSimulationLibrary.GetNumPlayers(Context);

    public uint LocalPlayer => CSimulationLibrary.GetLocalPlayer(Context);

    // Player's input for this tick, a copy of a plain struct or a view of a script struct that lasts until the tick ends.
    public T GetInput<T>(uint Player) where T : struct
    {
        nint Address = CSimulationLibrary.GetInput(Context, Player, SimulationInput<T>.Name);
        if (Address == 0)
        {
            throw new InvalidOperationException($"No simulation input of {SimulationInput<T>.Name} for player {Player}; there is no session, or its input is another struct.");
        }
        return SimulationInput<T>.Read(Address);
    }

    // An entity with a SimId every peer agrees on. Only an exclusive system may spawn.
    public FEntity Spawn() => CSimulationLibrary.Spawn(Context);

    // Takes the entity's rollback state away now and destroys it once no rewind can reach this tick. Only an exclusive system may destroy.
    public void Destroy(FEntity Entity) => CSimulationLibrary.Destroy(Context, Entity);

    public FEntity? TryGetEntity(uint SimId)
    {
        FEntity Entity = CSimulationLibrary.FindEntity(Context, SimId);
        return Entity.IsNull ? null : Entity;
    }

    // Live simulation entities in SimId order, the one order every peer agrees on, unlike a view's.
    public FEntity[] GetEntities() => CSimulationLibrary.GetEntities(Context);

    // Draws from the session's seeded stream, which every snapshot saves, so a replay draws the same numbers.
    public uint RandomUInt32() => CSimulationLibrary.RandomUInt32(Context);

    // Uniform over [Min, Max].
    public int RandomRange(int Min, int Max) => CSimulationLibrary.RandomRange(Context, Min, Max);

    // Uniform over [Min, Max].
    public float RandomRange(float Min, float Max) => CSimulationLibrary.RandomFloatRange(Context, Min, Max);

    // Uniform over [0, 1).
    public float RandomFloat() => CSimulationLibrary.RandomFloat(Context);

    public bool RandomBool() => CSimulationLibrary.RandomBool(Context);
}

// The deterministic simulation as the game sees it outside the Simulation stage, for starting a session, feeding it local input and blending between ticks.
public readonly unsafe struct WorldSimulation
{
    private readonly CWorld World;

    internal WorldSimulation(CWorld World)
    {
        this.World = World;
    }

    // A standalone world plays alone, a server hands its ready clients a session, and a client joins the one its host starts.
    public bool Start<TInput>(ulong Seed) where TInput : struct => CSimulationLibrary.StartSession(World, SimulationInput<TInput>.Name, Seed);

    public bool IsRunning => CSimulationLibrary.IsSessionRunning(World);

    public ESimulationMode Mode => CSimulationLibrary.GetMode(World);

    public uint NumPlayers => CSimulationLibrary.GetSessionPlayers(World);

    public uint LocalPlayer => CSimulationLibrary.GetSessionLocalPlayer(World);

    // How far this frame sits between the last tick and the next, so rendering can blend two tick states instead of stepping.
    public float InterpolationAlpha => CSimulationLibrary.GetInterpolationAlpha(World);

    public uint CurrentTick => CSimulationLibrary.GetCurrentTick(World);

    // The newest tick every player's input is known for, from which nothing can be rewound.
    public long ConfirmedTick => CSimulationLibrary.GetConfirmedTick(World);

    public uint RollbackCount => CSimulationLibrary.GetRollbackCount(World);

    public bool HasDesynced => CSimulationLibrary.HasDesynced(World);

    // Holds Input for the next tick to be stamped and every tick after it until another arrives. False without a session or when the session's input is another struct.
    public bool SubmitInput<T>(in T Input) where T : unmanaged
    {
        if (SimulationInput<T>.bHandle)
        {
            throw new InvalidOperationException($"{typeof(T).Name} is a script struct, so fill it in place through GetLocalInput instead.");
        }
        nint Address = CSimulationLibrary.GetLocalInput(World, SimulationInput<T>.Name);
        if (Address == 0)
        {
            return false;
        }
        *(T*)Address = Input;
        return true;
    }

    // The held local input of a script struct session, filled in place.
    public T GetLocalInput<T>() where T : struct, INativeStruct<T>
        => TryGetLocalInput<T>() ?? throw new InvalidOperationException($"No simulation session takes {T.NativeName} as its input.");

    public T? TryGetLocalInput<T>() where T : struct, INativeStruct<T>
        => NativeStructs.FromAddressOrNull<T>(CSimulationLibrary.GetLocalInput(World, SimulationInput<T>.Name));
}

// The native name of one input type, and whether C# sees it as a value or as a view of native memory.
internal static unsafe class SimulationInput<T> where T : struct
{
    public static readonly FName Name = new(NativeTypeName.Of<T>());

    public static readonly bool bHandle = typeof(INativeStruct).IsAssignableFrom(typeof(T));

    public static T Read(nint Address)
    {
        if (bHandle)
        {
            NativeRef Ref = new(Address);
            return Unsafe.As<NativeRef, T>(ref Ref);
        }
        if (RuntimeHelpers.IsReferenceOrContainsReferences<T>())
        {
            throw new InvalidOperationException($"{typeof(T).Name} holds references, so it cannot be a simulation input.");
        }
        return Unsafe.ReadUnaligned<T>((void*)Address);
    }
}

public unsafe partial class CWorld
{
    public WorldSimulation Simulation => new(this);
}
