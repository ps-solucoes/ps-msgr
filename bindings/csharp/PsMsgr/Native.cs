// SPDX-License-Identifier: Apache-2.0
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace PsMsgr;

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeOptions
{
    public uint StructSize;
    public uint Capacity;
    public uint SlotCount;
    public uint PayloadType;
    public uint Mode;
    public uint Flags;
    public byte* Dir;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeInfo
{
    public uint Generation;
    public uint Length;
    public ulong TimestampNs;
    public uint Flags;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeDesc
{
    public uint Capacity;
    public uint SlotCount;
    public uint PayloadType;
    public uint Flags;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeWaitSetEvent
{
    public ulong Token;
    public int Status;
    public uint Generation;
    public int SysErrno;
    public uint Reserved;
}

/// <summary>
/// libpsmsgr.so.1 (include/psmsgr/psmsgr.h, state.h, waitset.h). Blittable signatures
/// only, so that neither the JIT nor Native AOT generates marshalling code.
/// </summary>
internal static unsafe class Native
{
    public const string Library = "libpsmsgr.so.1";
    public const uint VersionMajor = 1;
    public const uint MinVersionMinor = 1;

    public const int Ok = 0;
    public const int NameMax = 64;
    public const uint StateMaxCapacity = 16u << 20;
    public const uint StateDefaultSlots = 3;
    public const uint StateRecreate = 1u << 0;
    public const uint StateNoNotify = 1u << 1;
    public const uint InfoAttached = 1u << 0;
    public const int WaitSetMax = 127;

    private const int RtldNow = 0x2;
    private const int RtldGlobal = 0x100;

    // Runs before the first call into the library. With PSMSGR_LIBRARY set,
    // the library is loaded from there first: glibc then resolves the
    // DllImport of the same SONAME to the loaded copy.
    static Native()
    {
        string? path = Environment.GetEnvironmentVariable("PSMSGR_LIBRARY");
        if (string.IsNullOrEmpty(path))
            path = null;
        else
            Preload(path!);
        uint version;
        try
        {
            version = psmsgr_version();
        }
        catch (DllNotFoundException e)
        {
            throw new DllNotFoundException(
                $"PsMsgr cannot load {Library}: {e.Message} (install libpsmsgr1, or set "
                + "PSMSGR_LIBRARY to the path of libpsmsgr.so.1)", e);
        }
        catch (EntryPointNotFoundException e)
        {
            throw new DllNotFoundException($"PsMsgr: {path ?? Library} is not libpsmsgr: {e.Message}", e);
        }
        CheckVersion(version, path ?? Library);
    }

    private static void Preload(string path)
    {
        byte[] file = Utf8z(path);
        // Binds dlerror now: binding it after a failed dlopen would clear the error, and
        // so could managed code running in between.
        dlerror();
        IntPtr handle;
        byte* error = null;
        fixed (byte* p = file)
        {
            handle = dlopen(p, RtldNow | RtldGlobal);
            if (handle == IntPtr.Zero)
                error = dlerror();
        }
        if (handle == IntPtr.Zero)
            throw new DllNotFoundException($"PsMsgr cannot load {path} (PSMSGR_LIBRARY): {FromUtf8z(error)}");
    }

    internal static void CheckVersion(uint version, string path)
    {
        uint major = version >> 16, minor = (version >> 8) & 0xFF, patch = version & 0xFF;
        if (major != VersionMajor || minor < MinVersionMinor)
        {
            throw new PsMsgrException(
                PsMsgrError.NotSup,
                $"PsMsgr needs libpsmsgr {VersionMajor}.{MinVersionMinor} or a later {VersionMajor}.x, "
                + $"but {path} is {major}.{minor}.{patch}");
        }
    }

    internal static byte[] Utf8z(string s)
    {
        var b = new byte[Encoding.UTF8.GetByteCount(s) + 1];
        Encoding.UTF8.GetBytes(s, 0, s.Length, b, 0);
        return b;
    }

    internal static string FromUtf8z(byte* s)
    {
        if (s == null)
            return "";
        int n = 0;
        while (s[n] != 0)
            n++;
        return Encoding.UTF8.GetString(s, n);
    }

    [DllImport("libdl.so.2")]
    private static extern IntPtr dlopen(byte* file, int mode);

    [DllImport("libdl.so.2")]
    private static extern byte* dlerror();

    // The plain glibc symbol is the GNU variant, which may ignore the buffer. The XSI one
    // still writes "Unknown error N" when it returns EINVAL, so callers check the buffer.
    [DllImport("libc.so.6", EntryPoint = "__xpg_strerror_r")]
    internal static extern int strerror_r(int errnum, byte* buf, nuint buflen);

    [DllImport(Library)]
    internal static extern uint psmsgr_version();

    [DllImport(Library)]
    internal static extern byte* psmsgr_strerror(int code);

    [DllImport(Library)]
    internal static extern ulong psmsgr_now_ns();

    [DllImport(Library)]
    internal static extern void psmsgr_state_options_init_sized(NativeOptions* opt, uint size);

    [DllImport(Library, SetLastError = true)]
    internal static extern int psmsgr_state_writer_open(byte* name, NativeOptions* opt, IntPtr* w);

    [DllImport(Library)]
    internal static extern void psmsgr_state_writer_close(IntPtr w);

    [DllImport(Library)]
    internal static extern int psmsgr_state_publish(IntPtr w, void* data, uint len, uint* generation);

    [DllImport(Library)]
    internal static extern int psmsgr_state_begin(IntPtr w, void** buf);

    [DllImport(Library)]
    internal static extern int psmsgr_state_commit(IntPtr w, uint len, uint* generation);

    [DllImport(Library)]
    internal static extern int psmsgr_state_abort(IntPtr w);

    [DllImport(Library)]
    internal static extern uint psmsgr_state_writer_capacity(IntPtr w);

    [DllImport(Library, SetLastError = true)]
    internal static extern int psmsgr_state_reader_open(byte* name, byte* dir, IntPtr* r);

    [DllImport(Library)]
    internal static extern void psmsgr_state_reader_close(IntPtr r);

    [DllImport(Library, SetLastError = true)]
    internal static extern int psmsgr_state_read(IntPtr r, void* buf, uint size, NativeInfo* info);

    [DllImport(Library, SetLastError = true)]
    internal static extern int psmsgr_state_peek(IntPtr r, NativeInfo* info);

    [DllImport(Library, SetLastError = true)]
    internal static extern int psmsgr_state_wait(IntPtr r, uint lastGeneration, int timeoutMs);

    [DllImport(Library, SetLastError = true)]
    internal static extern int psmsgr_state_writer_alive(IntPtr r);

    [DllImport(Library, SetLastError = true)]
    internal static extern int psmsgr_state_describe_sized(IntPtr r, NativeDesc* desc, uint size);

    [DllImport(Library, SetLastError = true)]
    internal static extern int psmsgr_state_unlink(byte* name, byte* dir);

    [DllImport(Library, SetLastError = true)]
    internal static extern int psmsgr_waitset_open(IntPtr* ws);

    [DllImport(Library)]
    internal static extern void psmsgr_waitset_close(IntPtr ws);

    [DllImport(Library)]
    internal static extern int psmsgr_waitset_add(IntPtr ws, IntPtr r, uint lastGeneration, ulong token);

    [DllImport(Library)]
    internal static extern int psmsgr_waitset_remove(IntPtr ws, IntPtr r);

    [DllImport(Library, SetLastError = true)]
    internal static extern int psmsgr_waitset_wait(IntPtr ws, int timeoutMs, NativeWaitSetEvent* events, uint cap, uint* n);

    [DllImport(Library)]
    internal static extern void psmsgr_waitset_wake(IntPtr ws);
}

internal sealed class WriterHandle : SafeHandle
{
    public WriterHandle() : base(IntPtr.Zero, true) { }

    public override bool IsInvalid => handle == IntPtr.Zero;

    internal void Set(IntPtr w) => SetHandle(w);

    protected override bool ReleaseHandle()
    {
        Native.psmsgr_state_writer_close(handle);
        return true;
    }
}

internal sealed class ReaderHandle : SafeHandle
{
    public ReaderHandle() : base(IntPtr.Zero, true) { }

    public override bool IsInvalid => handle == IntPtr.Zero;

    internal void Set(IntPtr r) => SetHandle(r);

    protected override bool ReleaseHandle()
    {
        Native.psmsgr_state_reader_close(handle);
        return true;
    }
}
