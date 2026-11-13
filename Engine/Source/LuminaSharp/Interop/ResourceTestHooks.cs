using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

internal struct TestScore : IResource
{
    public int Value;
}

#pragma warning disable LUM0112
internal sealed class ResourceReaderTestSystem : CEntitySystem
{
    public TRes<SWorldSettingsState> Settings = new();

    public TRes<TestScore> Score = new();
}

internal sealed class ResourceWriterTestSystem : CEntitySystem
{
    public TResMut<TestScore> Score = new();
}

internal sealed class WeatherWriterTestSystem : CEntitySystem
{
    public TResMut<SWeatherState> Weather = new();
}
#pragma warning restore LUM0112

internal static unsafe class ResourceTestHooks
{
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr Test_ResourcesBind(IntPtr NativeSystem, int Kind)
    {
        CEntitySystem Tested = Kind switch
        {
            0 => new ResourceReaderTestSystem(),
            1 => new ResourceWriterTestSystem(),
            _ => new WeatherWriterTestSystem(),
        };
        Tested.BindNativeHandle(NativeSystem);
        Tested.BindSystemParams();
        return GCHandle.ToIntPtr(GCHandle.Alloc(Tested));
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_ResourcesRun(IntPtr Handle, float* OutValue)
    {
        switch (GCHandle.FromIntPtr(Handle).Target)
        {
            case ResourceReaderTestSystem Reader:
                OutValue[0] = Reader.Settings.Value.Gravity.Y;
                OutValue[1] = Reader.Score.Value.Value;
                return 1;
            case ResourceWriterTestSystem Writer:
                Writer.Score.Value.Value += 5;
                OutValue[0] = Writer.Score.Value.Value;
                return 1;
            case WeatherWriterTestSystem Weather:
                Weather.Weather.Value.Wetness = 0.75f;
                return 1;
            default:
                return 0;
        }
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_ResourcesRelease(IntPtr Handle)
    {
        GCHandle.FromIntPtr(Handle).Free();
    }
}
