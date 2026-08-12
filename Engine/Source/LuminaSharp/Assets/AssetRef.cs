using System;

namespace LuminaSharp
{
    /// <summary>
    /// Serializer contract: read/write any asset-reference type as a virtual path string. Implemented
    /// (explicitly) by the Lumina.* asset-reference types so the [Property] serializer round-trips them as
    /// a path + AssetType picker, exactly like a reflected C++ TObjectPtr/TSoftObjectPtr/FSoftObjectPath.
    /// </summary>
    internal interface IAssetRef
    {
        string GetPath();
        void SetFromPath(string Path);
    }
}

namespace Lumina
{
    using LuminaSharp;

    /// <summary>
    /// C# mirror of <c>Lumina::FSoftObjectPath</c>: a serializable asset reference by virtual path that
    /// resolves on demand and never force-loads. As a script [Property] it shows an asset picker.
    /// </summary>
    public struct FSoftObjectPath : IAssetRef
    {
        public string Path;

        public FSoftObjectPath(string Path)
        {
            this.Path = Path ?? "";
        }

        public readonly bool IsValid => !string.IsNullOrEmpty(Path);

        /// <summary>Registry probe (no load).</summary>
        public readonly bool Exists()
        {
            return IsValid && Asset.Exists(Path);
        }

        /// <summary>Blocking load, typed. Null if the path is empty or the asset can't be loaded.</summary>
        public readonly T? Load<T>() where T : NativeObject
        {
            return IsValid ? Asset.Load<T>(Path) : null;
        }

        /// <summary>Async load, typed; the callback runs on the game thread (once).</summary>
        public readonly void LoadAsync<T>(Action<T?> Callback) where T : NativeObject
        {
            if (IsValid)
            {
                Asset.LoadAsync(Path, Callback);
            }
            else
            {
                Callback(null);
            }
        }

        readonly string IAssetRef.GetPath()
        {
            return Path ?? "";
        }

        void IAssetRef.SetFromPath(string NewPath)
        {
            Path = NewPath ?? "";
        }
    }

    /// <summary>
    /// C# mirror of <c>Lumina::TSoftObjectPtr&lt;T&gt;</c>: a typed soft reference (a path that resolves to
    /// T on demand). Loads are asset-manager-cached, so <see cref="Get"/> is cheap once loaded.
    /// </summary>
    public struct TSoftObjectPtr<T> : IAssetRef where T : NativeObject
    {
        public FSoftObjectPath Path;

        public TSoftObjectPtr(string Path)
        {
            this.Path = new FSoftObjectPath(Path);
        }

        public readonly bool IsValid => Path.IsValid;

        /// <summary>Resolves + loads (blocking) to T, or null.</summary>
        public readonly T? Get()
        {
            return Path.Load<T>();
        }

        /// <summary>Async resolve; the callback runs on the game thread (once).</summary>
        public readonly void LoadAsync(Action<T?> Callback)
        {
            Path.LoadAsync(Callback);
        }

        readonly string IAssetRef.GetPath()
        {
            return Path.Path ?? "";
        }

        void IAssetRef.SetFromPath(string NewPath)
        {
            Path = new FSoftObjectPath(NewPath);
        }
    }

    /// <summary>
    /// C# mirror of <c>Lumina::TObjectPtr&lt;T&gt;</c>: a typed strong reference holding the resolved object.
    /// As a script [Property] it serializes via the object's asset path and eager-loads on apply; in code
    /// it carries the loaded object (assign it straight to a component property).
    /// </summary>
    public struct TObjectPtr<T> : IAssetRef where T : NativeObject
    {
        private IntPtr Handle; // resolved CObject*, or zero

        public TObjectPtr(T? Value)
        {
            Handle = Value != null ? Value.Handle : IntPtr.Zero;
        }

        public readonly bool IsValid => Handle != IntPtr.Zero;

        /// <summary>The resolved object as T, or null.</summary>
        public readonly T? Value => Handle == IntPtr.Zero ? null : Wrapper<T>.ForObject(Handle);


        public readonly T? Get()
        {
            return Value;
        }

        public static implicit operator T?(TObjectPtr<T> Pointer)
        {
            return Pointer.Value;
        }

        readonly string IAssetRef.GetPath()
        {
            return Handle == IntPtr.Zero ? "" : Native.GetObjectPath(Handle);
        }

        void IAssetRef.SetFromPath(string NewPath)
        {
            Handle = string.IsNullOrEmpty(NewPath) ? IntPtr.Zero : Native.LoadObject(NewPath);
        }
    }
}
