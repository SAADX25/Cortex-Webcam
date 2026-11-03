using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;
using System.Windows.Interop;

namespace CortexStudio;

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
internal struct NativeState
{
    public int Running;
    public int Ready;
    public int HasFrame;
    public int VirtualEnabled;
    public int Mode;
    public int Width;
    public int Height;
    public int OutputFps;
    public double IncomingFps;
    public ulong Frames;
    public ulong Packets;
    public ulong Gaps;
    public ulong VirtualSent;
    public ulong VirtualSkipped;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
    public string Status;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)]
    public string Error;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
    public string Discovery;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
    public string Peer;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)]
    public string Diagnostics;

    public int PhoneFps;
    public int PhoneSyncAvailable;
    public int RequestedPhoneFps;
    public int FrameRatePending;
    public int PhoneRateFallback;
    public ulong FrameRateRequestId;
    public ulong FrameRateAckId;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
    public string DeviceName;
}

internal static class Native
{
    [DllImport("CortexStudioNative", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern nint cortex_studio_create(nint preview, string model);

    [DllImport("CortexStudioNative", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void cortex_studio_destroy(nint handle);

    [DllImport("CortexStudioNative", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int cortex_studio_get_state(nint handle, out NativeState state, int bytes);

    [DllImport("CortexStudioNative", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void cortex_studio_set(nint handle, int property, double value);

    [DllImport("CortexStudioNative", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    internal static extern void cortex_studio_path(nint handle, int kind, string path);
}

public sealed class NativePreview : HwndHost
{
    private nint _window;
    private nint _engine;

    public event Action? EngineCreated;
    public event Action<string>? EngineError;

    internal string ModelPath { get; set; } = "";
    internal bool HasEngine => _engine != 0;

    protected override HandleRef BuildWindowCore(HandleRef parent)
    {
        _window = CreateWindowEx(
            0,
            "static",
            "",
            0x40000000 | 0x02000000 | 0x04000000 | 0x00000004,
            0,
            0,
            1,
            1,
            parent.Handle,
            0,
            0,
            0);

        if (_window == 0)
        {
            throw new InvalidOperationException("Could not create the preview surface.");
        }

        try
        {
            _engine = Native.cortex_studio_create(_window, ModelPath);
            if (_engine == 0)
            {
                throw new InvalidOperationException("Could not start the video engine.");
            }
            Dispatcher.InvokeAsync(() => EngineCreated?.Invoke());
        }
        catch (Exception error)
        {
            Dispatcher.InvokeAsync(() => EngineError?.Invoke(error.Message));
        }

        return new HandleRef(this, _window);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        StopEngine();
        DestroyWindow(hwnd.Handle);
        _window = 0;
    }

    protected override void OnWindowPositionChanged(Rect bounds)
    {
        base.OnWindowPositionChanged(bounds);
        if (_window != 0)
        {
            InvalidateRect(_window, 0, false);
        }
    }

    [DllImport("user32.dll")]
    private static extern bool InvalidateRect(nint window, nint rect, bool erase);

    internal void Set(int property, double value)
    {
        if (_engine != 0)
        {
            Native.cortex_studio_set(_engine, property, value);
        }
    }

    internal void SetPath(int kind, string path)
    {
        if (_engine != 0)
        {
            Native.cortex_studio_path(_engine, kind, path);
        }
    }

    internal NativeState? Snapshot()
    {
        return _engine != 0 && Native.cortex_studio_get_state(_engine, out var state, Marshal.SizeOf<NativeState>()) != 0
            ? state
            : null;
    }

    internal void StopEngine()
    {
        var engine = Interlocked.Exchange(ref _engine, 0);
        if (engine != 0)
        {
            Native.cortex_studio_destroy(engine);
        }
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern nint CreateWindowEx(
        int exStyle,
        string className,
        string title,
        int style,
        int x,
        int y,
        int width,
        int height,
        nint parent,
        nint menu,
        nint instance,
        nint param);

    [DllImport("user32.dll")]
    private static extern bool DestroyWindow(nint hwnd);
}
