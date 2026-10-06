using System;

namespace LuminaSharp
{
    // Every type a script [Property] stores natively as an FSoftObjectPath, recognized by this interface rather than by name.
    public interface ISoftObjectReference
    {
        string GetPath();
        void SetFromPath(string Path);
    }

    // The struct constraint compiles the interface calls as constrained ones, so Read sets the value it returns, not a box.
    public static class SoftObjectReferenceMarshal
    {
        public static T Read<T>(string Path) where T : struct, ISoftObjectReference
        {
            T Value = default;
            Value.SetFromPath(Path ?? "");
            return Value;
        }

        public static string Write<T>(T Value) where T : struct, ISoftObjectReference
        {
            return Value.GetPath() ?? "";
        }
    }
}

namespace Lumina
{
    using LuminaSharp;

    // Mirror of FSoftObjectPath, a reference by virtual path that resolves on demand and never force-loads.
    public struct FSoftObjectPath : ISoftObjectReference
    {
        public string Path;

        public FSoftObjectPath(string Path)
        {
            this.Path = Path ?? "";
        }

        public readonly bool IsNull => string.IsNullOrEmpty(Path);

        public readonly bool IsValid => !IsNull;

        public readonly string GetPath()
        {
            return Path ?? "";
        }

        // True when the registry knows the path, without loading anything.
        public readonly bool TryResolve()
        {
            return IsValid && Native.AssetExists(Path);
        }

        public readonly T? LoadSynchronous<T>() where T : NativeObject
        {
            return IsValid ? ObjectCore.LoadObject<T>(Path) : null;
        }

        public readonly void LoadAsync<T>(Action<T?> Callback) where T : NativeObject
        {
            if (IsValid)
            {
                ObjectCore.AsyncLoadObject(Path, Callback);
            }
            else
            {
                Callback(null);
            }
        }

        void ISoftObjectReference.SetFromPath(string NewPath)
        {
            Path = NewPath ?? "";
        }
    }

    // Mirror of TSoftObjectPtr, a typed soft reference that resolves to T on demand.
    public struct TSoftObjectPtr<T> : ISoftObjectReference where T : NativeObject
    {
        public FSoftObjectPath Path;

        public TSoftObjectPtr(string Path)
        {
            this.Path = new FSoftObjectPath(Path);
        }

        public readonly bool IsNull => Path.IsNull;

        public readonly bool IsValid => Path.IsValid;

        public readonly string GetPath()
        {
            return Path.GetPath();
        }

        public readonly T? LoadSynchronous()
        {
            return Path.LoadSynchronous<T>();
        }

        public readonly void LoadAsync(Action<T?> Callback)
        {
            Path.LoadAsync(Callback);
        }

        void ISoftObjectReference.SetFromPath(string NewPath)
        {
            Path = new FSoftObjectPath(NewPath);
        }
    }

    // Mirror of TStrongObjectPtr, a hard reference stored as an object property so it keeps the object alive and needs no path.
    public struct TStrongObjectPtr<T> where T : NativeObject
    {
        private IntPtr Handle;

        public TStrongObjectPtr(T? Value)
        {
            Handle = Value != null ? Value.Handle : IntPtr.Zero;
        }

        // Wraps an already resolved native pointer, as the accessors ScriptPropertyRewriter emits do.
        public TStrongObjectPtr(IntPtr NativeObject)
        {
            Handle = NativeObject;
        }

        public readonly IntPtr NativeHandle => Handle;

        public readonly bool IsValid => Handle != IntPtr.Zero;

        public readonly T? Value => Handle == IntPtr.Zero ? null : Wrapper<T>.ForObject(Handle);

        public readonly T? Get()
        {
            return Value;
        }

        public static implicit operator T?(TStrongObjectPtr<T> Pointer)
        {
            return Pointer.Value;
        }
    }
}
