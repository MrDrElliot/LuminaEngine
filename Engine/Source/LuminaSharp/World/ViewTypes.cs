using System;
using System.Buffers;

namespace LuminaSharp;

// typed typed views authored in C#; native gathers entities + live component pointers in chunks (one crossing per chunk).
// WRAPPER LIFETIME: wrappers handed back are valid only for the current iteration step; do not store them, copy out fields you need.

internal static class ViewConst
{
    public const int ChunkSize = 1024; // entities gathered per native crossing
}

/// A typed typed view over one component type. Each + foreach.
public readonly unsafe struct View<T1>
    where T1 : NativeStruct
{
    private readonly ulong World;
    private readonly IntPtr Token0;
    private readonly Exclude Filter;

    internal View(ulong World, IntPtr Token0, Exclude Filter)
    {
        this.World = World;
        this.Token0 = Token0;
        this.Filter = Filter;
    }

    public void Each(Action<Entity, T1> Body)
    {
        const int N = 1;
        T1 W0 = ViewWrapper<T1>.New();

        uint[] Entities = ArrayPool<uint>.Shared.Rent(ViewConst.ChunkSize);
        IntPtr[] Ptrs = ArrayPool<IntPtr>.Shared.Rent(ViewConst.ChunkSize * N);
        IntPtr State = ViewBegin(World, Token0, Filter);
        if (State == IntPtr.Zero)
        {
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
            return;
        }

        try
        {
            fixed (uint* E = Entities)
            fixed (IntPtr* P = Ptrs)
            {
                int Count;
                while ((Count = Native.ViewNextChunk(State, E, P, ViewConst.ChunkSize, N)) > 0)
                {
                    for (int i = 0; i < Count; ++i)
                    {
                        W0.Handle = P[i * N + 0];
                        Body(new Entity(E[i]), W0);
                    }
                }
            }
        }
        finally
        {
            Native.ViewEnd(State);
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
        }
    }

    public Enumerator GetEnumerator() => new(World, Token0, Filter);

    public unsafe struct Enumerator : IDisposable
    {
        private const int N = 1;
        private readonly T1 W0;
        private readonly uint[] Entities;
        private readonly IntPtr[] Ptrs;
        private IntPtr State;
        private int Count;
        private int Index;

        internal Enumerator(ulong World, IntPtr Token0, Exclude Filter)
        {
            W0 = ViewWrapper<T1>.New();
            Entities = ArrayPool<uint>.Shared.Rent(ViewConst.ChunkSize);
            Ptrs = ArrayPool<IntPtr>.Shared.Rent(ViewConst.ChunkSize * N);
            State = ViewBegin(World, Token0, Filter);
            Count = 0;
            Index = -1;
        }

        public (Entity, T1) Current
        {
            get
            {
                int i = Index;
                W0.Handle = Ptrs[i * N + 0];
                return (new Entity(Entities[i]), W0);
            }
        }

        public bool MoveNext()
        {
            if (State == IntPtr.Zero)
            {
                return false;
            }

            if (++Index < Count)
            {
                return true;
            }

            fixed (uint* E = Entities)
            fixed (IntPtr* P = Ptrs)
            {
                Count = Native.ViewNextChunk(State, E, P, ViewConst.ChunkSize, N);
            }
            Index = 0;
            return Count > 0;
        }

        public void Dispose()
        {
            if (State != IntPtr.Zero)
            {
                Native.ViewEnd(State);
                State = IntPtr.Zero;
            }
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
        }
    }

    private static IntPtr ViewBegin(ulong World, IntPtr Token0, Exclude Filter)
    {
        IntPtr* Inc = stackalloc IntPtr[1] { Token0 };
        IntPtr* Exc = stackalloc IntPtr[3] { Filter.Token0, Filter.Token1, Filter.Token2 };
        return Native.ViewBegin(World, Inc, 1, Exc, Filter.Count);
    }
}

/// A typed typed view over two component types. Each + foreach.
public readonly unsafe struct View<T1, T2>
    where T1 : NativeStruct
    where T2 : NativeStruct
{
    private readonly ulong World;
    private readonly IntPtr Token0;
    private readonly IntPtr Token1;
    private readonly Exclude Filter;

    internal View(ulong World, IntPtr Token0, IntPtr Token1, Exclude Filter)
    {
        this.World = World;
        this.Token0 = Token0;
        this.Token1 = Token1;
        this.Filter = Filter;
    }

    public void Each(Action<Entity, T1, T2> Body)
    {
        const int N = 2;
        T1 W0 = ViewWrapper<T1>.New();
        T2 W1 = ViewWrapper<T2>.New();

        uint[] Entities = ArrayPool<uint>.Shared.Rent(ViewConst.ChunkSize);
        IntPtr[] Ptrs = ArrayPool<IntPtr>.Shared.Rent(ViewConst.ChunkSize * N);
        IntPtr State = ViewBegin(World, Token0, Token1, Filter);
        if (State == IntPtr.Zero)
        {
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
            return;
        }

        try
        {
            fixed (uint* E = Entities)
            fixed (IntPtr* P = Ptrs)
            {
                int Count;
                while ((Count = Native.ViewNextChunk(State, E, P, ViewConst.ChunkSize, N)) > 0)
                {
                    for (int i = 0; i < Count; ++i)
                    {
                        W0.Handle = P[i * N + 0];
                        W1.Handle = P[i * N + 1];
                        Body(new Entity(E[i]), W0, W1);
                    }
                }
            }
        }
        finally
        {
            Native.ViewEnd(State);
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
        }
    }

    public Enumerator GetEnumerator() => new(World, Token0, Token1, Filter);

    public unsafe struct Enumerator : IDisposable
    {
        private const int N = 2;
        private readonly T1 W0;
        private readonly T2 W1;
        private readonly uint[] Entities;
        private readonly IntPtr[] Ptrs;
        private IntPtr State;
        private int Count;
        private int Index;

        internal Enumerator(ulong World, IntPtr Token0, IntPtr Token1, Exclude Filter)
        {
            W0 = ViewWrapper<T1>.New();
            W1 = ViewWrapper<T2>.New();
            Entities = ArrayPool<uint>.Shared.Rent(ViewConst.ChunkSize);
            Ptrs = ArrayPool<IntPtr>.Shared.Rent(ViewConst.ChunkSize * N);
            State = ViewBegin(World, Token0, Token1, Filter);
            Count = 0;
            Index = -1;
        }

        public (Entity, T1, T2) Current
        {
            get
            {
                int i = Index;
                W0.Handle = Ptrs[i * N + 0];
                W1.Handle = Ptrs[i * N + 1];
                return (new Entity(Entities[i]), W0, W1);
            }
        }

        public bool MoveNext()
        {
            if (State == IntPtr.Zero)
            {
                return false;
            }

            if (++Index < Count)
            {
                return true;
            }

            fixed (uint* E = Entities)
            fixed (IntPtr* P = Ptrs)
            {
                Count = Native.ViewNextChunk(State, E, P, ViewConst.ChunkSize, N);
            }
            Index = 0;
            return Count > 0;
        }

        public void Dispose()
        {
            if (State != IntPtr.Zero)
            {
                Native.ViewEnd(State);
                State = IntPtr.Zero;
            }
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
        }
    }

    private static IntPtr ViewBegin(ulong World, IntPtr Token0, IntPtr Token1, Exclude Filter)
    {
        IntPtr* Inc = stackalloc IntPtr[2] { Token0, Token1 };
        IntPtr* Exc = stackalloc IntPtr[3] { Filter.Token0, Filter.Token1, Filter.Token2 };
        return Native.ViewBegin(World, Inc, 2, Exc, Filter.Count);
    }
}

/// A typed typed view over three component types. Each + foreach.
public readonly unsafe struct View<T1, T2, T3>
    where T1 : NativeStruct
    where T2 : NativeStruct
    where T3 : NativeStruct
{
    private readonly ulong World;
    private readonly IntPtr Token0;
    private readonly IntPtr Token1;
    private readonly IntPtr Token2;
    private readonly Exclude Filter;

    internal View(ulong World, IntPtr Token0, IntPtr Token1, IntPtr Token2, Exclude Filter)
    {
        this.World = World;
        this.Token0 = Token0;
        this.Token1 = Token1;
        this.Token2 = Token2;
        this.Filter = Filter;
    }

    public void Each(Action<Entity, T1, T2, T3> Body)
    {
        const int N = 3;
        T1 W0 = ViewWrapper<T1>.New();
        T2 W1 = ViewWrapper<T2>.New();
        T3 W2 = ViewWrapper<T3>.New();

        uint[] Entities = ArrayPool<uint>.Shared.Rent(ViewConst.ChunkSize);
        IntPtr[] Ptrs = ArrayPool<IntPtr>.Shared.Rent(ViewConst.ChunkSize * N);
        IntPtr State = ViewBegin(World, Token0, Token1, Token2, Filter);
        if (State == IntPtr.Zero)
        {
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
            return;
        }

        try
        {
            fixed (uint* E = Entities)
            fixed (IntPtr* P = Ptrs)
            {
                int Count;
                while ((Count = Native.ViewNextChunk(State, E, P, ViewConst.ChunkSize, N)) > 0)
                {
                    for (int i = 0; i < Count; ++i)
                    {
                        W0.Handle = P[i * N + 0];
                        W1.Handle = P[i * N + 1];
                        W2.Handle = P[i * N + 2];
                        Body(new Entity(E[i]), W0, W1, W2);
                    }
                }
            }
        }
        finally
        {
            Native.ViewEnd(State);
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
        }
    }

    public Enumerator GetEnumerator() => new(World, Token0, Token1, Token2, Filter);

    public unsafe struct Enumerator : IDisposable
    {
        private const int N = 3;
        private readonly T1 W0;
        private readonly T2 W1;
        private readonly T3 W2;
        private readonly uint[] Entities;
        private readonly IntPtr[] Ptrs;
        private IntPtr State;
        private int Count;
        private int Index;

        internal Enumerator(ulong World, IntPtr Token0, IntPtr Token1, IntPtr Token2, Exclude Filter)
        {
            W0 = ViewWrapper<T1>.New();
            W1 = ViewWrapper<T2>.New();
            W2 = ViewWrapper<T3>.New();
            Entities = ArrayPool<uint>.Shared.Rent(ViewConst.ChunkSize);
            Ptrs = ArrayPool<IntPtr>.Shared.Rent(ViewConst.ChunkSize * N);
            State = ViewBegin(World, Token0, Token1, Token2, Filter);
            Count = 0;
            Index = -1;
        }

        public (Entity, T1, T2, T3) Current
        {
            get
            {
                int i = Index;
                W0.Handle = Ptrs[i * N + 0];
                W1.Handle = Ptrs[i * N + 1];
                W2.Handle = Ptrs[i * N + 2];
                return (new Entity(Entities[i]), W0, W1, W2);
            }
        }

        public bool MoveNext()
        {
            if (State == IntPtr.Zero)
            {
                return false;
            }

            if (++Index < Count)
            {
                return true;
            }

            fixed (uint* E = Entities)
            fixed (IntPtr* P = Ptrs)
            {
                Count = Native.ViewNextChunk(State, E, P, ViewConst.ChunkSize, N);
            }
            Index = 0;
            return Count > 0;
        }

        public void Dispose()
        {
            if (State != IntPtr.Zero)
            {
                Native.ViewEnd(State);
                State = IntPtr.Zero;
            }
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
        }
    }

    private static IntPtr ViewBegin(ulong World, IntPtr Token0, IntPtr Token1, IntPtr Token2, Exclude Filter)
    {
        IntPtr* Inc = stackalloc IntPtr[3] { Token0, Token1, Token2 };
        IntPtr* Exc = stackalloc IntPtr[3] { Filter.Token0, Filter.Token1, Filter.Token2 };
        return Native.ViewBegin(World, Inc, 3, Exc, Filter.Count);
    }
}

/// A typed typed view over four component types. Each + foreach.
public readonly unsafe struct View<T1, T2, T3, T4>
    where T1 : NativeStruct
    where T2 : NativeStruct
    where T3 : NativeStruct
    where T4 : NativeStruct
{
    private readonly ulong World;
    private readonly IntPtr Token0;
    private readonly IntPtr Token1;
    private readonly IntPtr Token2;
    private readonly IntPtr Token3;
    private readonly Exclude Filter;

    internal View(ulong World, IntPtr Token0, IntPtr Token1, IntPtr Token2, IntPtr Token3, Exclude Filter)
    {
        this.World = World;
        this.Token0 = Token0;
        this.Token1 = Token1;
        this.Token2 = Token2;
        this.Token3 = Token3;
        this.Filter = Filter;
    }

    public void Each(Action<Entity, T1, T2, T3, T4> Body)
    {
        const int N = 4;
        T1 W0 = ViewWrapper<T1>.New();
        T2 W1 = ViewWrapper<T2>.New();
        T3 W2 = ViewWrapper<T3>.New();
        T4 W3 = ViewWrapper<T4>.New();

        uint[] Entities = ArrayPool<uint>.Shared.Rent(ViewConst.ChunkSize);
        IntPtr[] Ptrs = ArrayPool<IntPtr>.Shared.Rent(ViewConst.ChunkSize * N);
        IntPtr State = ViewBegin(World, Token0, Token1, Token2, Token3, Filter);
        if (State == IntPtr.Zero)
        {
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
            return;
        }

        try
        {
            fixed (uint* E = Entities)
            fixed (IntPtr* P = Ptrs)
            {
                int Count;
                while ((Count = Native.ViewNextChunk(State, E, P, ViewConst.ChunkSize, N)) > 0)
                {
                    for (int i = 0; i < Count; ++i)
                    {
                        W0.Handle = P[i * N + 0];
                        W1.Handle = P[i * N + 1];
                        W2.Handle = P[i * N + 2];
                        W3.Handle = P[i * N + 3];
                        Body(new Entity(E[i]), W0, W1, W2, W3);
                    }
                }
            }
        }
        finally
        {
            Native.ViewEnd(State);
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
        }
    }

    public Enumerator GetEnumerator() => new(World, Token0, Token1, Token2, Token3, Filter);

    public unsafe struct Enumerator : IDisposable
    {
        private const int N = 4;
        private readonly T1 W0;
        private readonly T2 W1;
        private readonly T3 W2;
        private readonly T4 W3;
        private readonly uint[] Entities;
        private readonly IntPtr[] Ptrs;
        private IntPtr State;
        private int Count;
        private int Index;

        internal Enumerator(ulong World, IntPtr Token0, IntPtr Token1, IntPtr Token2, IntPtr Token3, Exclude Filter)
        {
            W0 = ViewWrapper<T1>.New();
            W1 = ViewWrapper<T2>.New();
            W2 = ViewWrapper<T3>.New();
            W3 = ViewWrapper<T4>.New();
            Entities = ArrayPool<uint>.Shared.Rent(ViewConst.ChunkSize);
            Ptrs = ArrayPool<IntPtr>.Shared.Rent(ViewConst.ChunkSize * N);
            State = ViewBegin(World, Token0, Token1, Token2, Token3, Filter);
            Count = 0;
            Index = -1;
        }

        public (Entity, T1, T2, T3, T4) Current
        {
            get
            {
                int i = Index;
                W0.Handle = Ptrs[i * N + 0];
                W1.Handle = Ptrs[i * N + 1];
                W2.Handle = Ptrs[i * N + 2];
                W3.Handle = Ptrs[i * N + 3];
                return (new Entity(Entities[i]), W0, W1, W2, W3);
            }
        }

        public bool MoveNext()
        {
            if (State == IntPtr.Zero)
            {
                return false;
            }

            if (++Index < Count)
            {
                return true;
            }

            fixed (uint* E = Entities)
            fixed (IntPtr* P = Ptrs)
            {
                Count = Native.ViewNextChunk(State, E, P, ViewConst.ChunkSize, N);
            }
            Index = 0;
            return Count > 0;
        }

        public void Dispose()
        {
            if (State != IntPtr.Zero)
            {
                Native.ViewEnd(State);
                State = IntPtr.Zero;
            }
            ArrayPool<uint>.Shared.Return(Entities);
            ArrayPool<IntPtr>.Shared.Return(Ptrs);
        }
    }

    private static IntPtr ViewBegin(ulong World, IntPtr Token0, IntPtr Token1, IntPtr Token2, IntPtr Token3, Exclude Filter)
    {
        IntPtr* Inc = stackalloc IntPtr[4] { Token0, Token1, Token2, Token3 };
        IntPtr* Exc = stackalloc IntPtr[3] { Filter.Token0, Filter.Token1, Filter.Token2 };
        return Native.ViewBegin(World, Inc, 4, Exc, Filter.Count);
    }
}
