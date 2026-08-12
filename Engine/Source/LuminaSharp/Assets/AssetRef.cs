using System;

namespace LuminaSharp
{
    /// <summary>
    /// Contract for every asset-reference type: read and write it as a virtual path string. Implemented
    /// (explicitly) by the Lumina.* asset-reference types so a [Property] round-trips as a path + AssetType
    /// picker, exactly like a reflected C++ TObjectPtr / TSoftObjectPtr / FSoftObjectPath.
    ///
    /// Public because it is also how ScriptPropertyRewriter recognises an asset reference: a script property
    /// of any type implementing this is stored natively as an FSoftObjectPath and marshalled through
    /// AssetRefMarshal, so adding an asset-reference type needs no change in the rewriter.
    /// </summary>
    public interface IAssetRef
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
    /// C# mirror of <c>Lumina::TObjectPtr&lt;T&gt;</c>: a typed HARD reference to a live CObject.
    ///
    /// Deliberately not an <see cref="IAssetRef"/>. A hard reference is stored natively as an object
    /// property, the same as a C++ TObjectPtr, so it keeps the object alive and can point at any CObject
    /// rather than only at something with an asset path. Use <see cref="TSoftObjectPtr{T}"/> when you want
    /// a path that resolves on demand.
    /// </summary>
    public struct TObjectPtr<T> where T : NativeObject
    {
        private IntPtr Handle; // resolved CObject*, or zero

        public TObjectPtr(T? Value)
        {
            Handle = Value != null ? Value.Handle : IntPtr.Zero;
        }

        /// <summary>Wraps an already-resolved native CObject pointer. Used by the accessors
        /// ScriptPropertyRewriter emits for an object [Property].</summary>
        public TObjectPtr(IntPtr NativeObject)
        {
            Handle = NativeObject;
        }

        /// <summary>The raw native pointer, for handing back to the engine.</summary>
        public readonly IntPtr NativeHandle => Handle;

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

    }
}
