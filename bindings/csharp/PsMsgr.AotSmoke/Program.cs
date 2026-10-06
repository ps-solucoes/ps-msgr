// SPDX-License-Identifier: Apache-2.0
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using PsMsgr;

// Calls every public API of PsMsgr, so that the Native AOT publish analyzes all of it.
// Exits with 0 and prints "ok" when every check holds. check.sh also runs it with the JIT.

// First, a bare P/Invoke: it has to run the static constructor that loads the library.
Check(Clock.NowNs() > 0, "Clock.NowNs");

string dir = Directory.CreateTempSubdirectory("psmsgr-smoke-").FullName;
try
{
    Run(dir);
}
finally
{
    Directory.Delete(dir, recursive: true);
}
Console.WriteLine("ok");
return 0;

static void Run(string dir)
{
    var options = new StateOptions(24)
    {
        SlotCount = 2,
        PayloadType = 0x0001_0001,
        Mode = 0x180,
        Recreate = true,
        Notify = true,
        Directory = dir,
    };
    Check(options.Capacity == 24 && options.SlotCount == 2 && options.PayloadType == 0x0001_0001
        && options.Mode == 0x180 && options.Recreate && options.Notify && options.Directory == dir, "options");

    using var reader = StateReader.Open("smoke", dir);
    Check(!reader.TryPeek(out _) && reader.Read(out _) is null && reader.Describe() is null, "lazy reader");
    Check(!reader.Wait(0, TimeSpan.Zero) && !reader.IsWriterAlive, "no writer");

    using (var writer = StateWriter.Open("smoke", options))
    {
        Check(writer.Capacity == 24 && reader.IsWriterAlive, "writer");
        try
        {
            StateWriter.Open("smoke", options).Dispose();
            Check(false, "second writer");
        }
        catch (PsMsgrException e)
        {
            Check(e.Code == PsMsgrError.WriterExists && e.Errno == 0 && e.ChannelName == "smoke"
                && e.Message.Length > 0 && e.Code.ToString() == "WriterExists", "WriterExists");
        }

        var status = new MotorStatus { Sequence = 1, SpeedRpm = 1500f, CurrentA = 2.5f, TemperatureC = 41f };
        uint gen = writer.Publish(status);
        Check(reader.Wait(0, TimeSpan.FromSeconds(1)), "wait");
        Check(reader.TryRead(out MotorStatus got, out StateInfo info) && got.Equals(status), "TryRead<T>");
        Check(info.Generation == gen && info.Length == 24 && info.Attached && info.TimestampNs <= Clock.NowNs()
            && info.Age >= TimeSpan.Zero && info.ToString().Contains("Generation"), "StateInfo");
        Check(info == info with { } && info.GetHashCode() == info.GetHashCode(), "StateInfo equality");
        var (generation, length, _, attached) = info;
        Check(generation == gen && length == 24 && attached, "StateInfo deconstruction");

        gen = writer.Publish(MemoryMarshal.AsBytes(new[] { status }.AsSpan()));
        byte[]? data = reader.Read(out info);
        Check(data is not null && MemoryMarshal.Read<MotorStatus>(data).Equals(status) && info.Generation == gen, "Read");

        using (var scope = writer.Begin())
        {
            scope.Buffer.Fill(0x5A);
            gen = scope.Commit(8);
        }
        using (var scope = writer.Begin())
            scope.Buffer.Clear(); // aborted
        var buf = new byte[24];
        Check(reader.TryRead(buf, out info) && info.Generation == gen && info.Length == 8 && buf[7] == 0x5A, "TryRead");
        Check(reader.TryPeek(out info) && info.Generation == gen, "TryPeek");

        ChannelDesc? desc = reader.Describe();
        Check(desc is { Capacity: 24, SlotCount: 2, PayloadType: 0x0001_0001, Notify: true }
            && desc.Value.ToString().Contains("Capacity"), "Describe");

        Task<bool> pending = reader.WaitAsync(gen, TimeSpan.FromSeconds(5));
        gen = writer.Publish(status);
        Check(pending.GetAwaiter().GetResult(), "WaitAsync");
        Check(!reader.WaitAsync(gen, TimeSpan.FromMilliseconds(10)).GetAwaiter().GetResult(), "WaitAsync timeout");
        using (var asyncCts = new CancellationTokenSource(TimeSpan.FromMilliseconds(50)))
        {
            try
            {
                reader.WaitAsync(gen, Timeout.InfiniteTimeSpan, asyncCts.Token).GetAwaiter().GetResult();
                Check(false, "WaitAsync cancellation");
            }
            catch (OperationCanceledException)
            {
            }
        }

        using var cts = new CancellationTokenSource(TimeSpan.FromMilliseconds(50));
        try
        {
            reader.Wait(gen, Timeout.InfiniteTimeSpan, cts.Token);
            Check(false, "cancellation");
        }
        catch (OperationCanceledException)
        {
        }
    }

    Check(!reader.IsWriterAlive && Channel.Unlink("smoke", dir) && !Channel.Unlink("smoke", dir), "Unlink");
    var error = new PsMsgrException(PsMsgrError.Sys, errno: 2, channelName: "smoke");
    Check(error.Code == PsMsgrError.Sys && error.Errno == 2 && error is IOException
        && error.Message == "No such file or directory (errno 2): 'smoke'", "PsMsgrException");
}

static void Check(bool condition, string what)
{
    if (!condition)
        throw new InvalidOperationException($"check failed: {what}");
}

/// <summary>Shaped like the C <c>struct motor_status</c> of the documentation.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct MotorStatus
{
    public ulong Sequence;
    public float SpeedRpm;
    public float CurrentA;
    public float TemperatureC;
}
