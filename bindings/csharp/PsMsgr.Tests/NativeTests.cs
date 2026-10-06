// SPDX-License-Identifier: Apache-2.0
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace PsMsgr.Tests;

/// <summary>The P/Invoke mirrors against the C headers (tests/interop_helper layout).</summary>
public sealed unsafe class NativeTests
{
    private static readonly Lazy<Dictionary<(string Kind, string Key), long>> CLayout = new(() =>
    {
        var layout = new Dictionary<(string, string), long>();
        foreach (string line in Tools.Run(Tools.BuildFile("tests/interop_helper"), "layout").Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            string[] f = line.Split(' ');
            layout[(f[0], f[1])] = long.Parse(f[2]);
        }
        return layout;
    });

    private static void AssertLayout<T>(string cName, params (string C, string Field)[] fields) where T : unmanaged
    {
        var layout = CLayout.Value;
        Assert.Equal(layout[("sizeof", cName)], Unsafe.SizeOf<T>());
        Assert.Equal(layout[("sizeof", cName)], Marshal.SizeOf<T>());
        var cOffsets = layout.Where(kv => kv.Key.Kind == "offsetof" && kv.Key.Key.StartsWith(cName + ".", StringComparison.Ordinal))
            .ToDictionary(kv => kv.Key.Key[(cName.Length + 1)..], kv => kv.Value);
        Assert.Equal(cOffsets.Keys.Order(), fields.Select(f => f.C).Order());
        foreach (var (c, field) in fields)
            Assert.Equal(cOffsets[c], (long)Marshal.OffsetOf<T>(field));
    }

    [Fact]
    public void StructLayouts()
    {
        AssertLayout<NativeOptions>("psmsgr_state_options",
            ("struct_size", "StructSize"), ("capacity", "Capacity"), ("slot_count", "SlotCount"),
            ("payload_type", "PayloadType"), ("mode", "Mode"), ("flags", "Flags"), ("dir", "Dir"));
        AssertLayout<NativeInfo>("psmsgr_state_info",
            ("generation", "Generation"), ("length", "Length"), ("timestamp_ns", "TimestampNs"),
            ("flags", "Flags"), ("reserved", "Reserved"));
        AssertLayout<NativeDesc>("psmsgr_state_desc",
            ("capacity", "Capacity"), ("slot_count", "SlotCount"), ("payload_type", "PayloadType"), ("flags", "Flags"));
        AssertLayout<NativeWaitSetEvent>("psmsgr_waitset_event",
            ("token", "Token"), ("status", "Status"), ("generation", "Generation"), ("sys_errno", "SysErrno"),
            ("reserved", "Reserved"));
    }

    [Fact]
    public void Constants()
    {
        var consts = CLayout.Value.Where(kv => kv.Key.Kind == "const").ToDictionary(kv => kv.Key.Key, kv => kv.Value);
        Assert.Equal(Native.VersionMajor, consts["PSMSGR_VERSION_MAJOR"]);
        Assert.True(consts["PSMSGR_VERSION_MINOR"] >= Native.MinVersionMinor);
        Assert.Equal(Native.Ok, consts["PSMSGR_OK"]);
        Assert.Equal(Native.NameMax, consts["PSMSGR_NAME_MAX"]);
        Assert.Equal(Native.StateMaxCapacity, consts["PSMSGR_STATE_MAX_CAPACITY"]);
        Assert.Equal(Native.StateDefaultSlots, consts["PSMSGR_STATE_DEFAULT_SLOTS"]);
        Assert.Equal(Native.StateRecreate, consts["PSMSGR_STATE_RECREATE"]);
        Assert.Equal(Native.StateNoNotify, consts["PSMSGR_STATE_NO_NOTIFY"]);
        Assert.Equal(Native.InfoAttached, consts["PSMSGR_INFO_ATTACHED"]);
        Assert.Equal(Native.WaitSetMax, consts["PSMSGR_WAITSET_MAX"]);

        var errors = consts.Where(kv => kv.Key.StartsWith("PSMSGR_E_", StringComparison.Ordinal))
            .ToDictionary(kv => kv.Key["PSMSGR_E_".Length..].Replace("_", ""), kv => kv.Value, StringComparer.OrdinalIgnoreCase);
        Assert.Equal(errors.Count, Enum.GetValues<PsMsgrError>().Length);
        foreach (PsMsgrError e in Enum.GetValues<PsMsgrError>())
            Assert.Equal(errors[e.ToString()], (long)e);
    }

    [Fact]
    public void OptionsInitSetsStructSize()
    {
        NativeOptions o;
        new Span<byte>(&o, sizeof(NativeOptions)).Fill(0xA5);
        Native.psmsgr_state_options_init_sized(&o, (uint)sizeof(NativeOptions));
        Assert.Equal((uint)sizeof(NativeOptions), o.StructSize);
        var defaults = new StateOptions(0);
        Assert.Equal((0u, defaults.SlotCount, defaults.PayloadType, defaults.Mode, 0u), (o.Capacity, o.SlotCount, o.PayloadType, o.Mode, o.Flags));
        Assert.True(o.Dir == null);
    }

    [Fact]
    public void VersionCheck()
    {
        Native.CheckVersion(Native.psmsgr_version(), "lib");
        Native.CheckVersion((1u << 16) | (Native.MinVersionMinor << 8) | 7, "lib");
        foreach (uint bad in new[] { 2u << 16, 0u << 16 | 9u << 8 })
        {
            var e = Assert.Throws<PsMsgrException>(() => Native.CheckVersion(bad, "lib"));
            Assert.Equal(PsMsgrError.NotSup, e.Code);
            Assert.Matches(@"^PsMsgr needs libpsmsgr 1\.\d+ or a later 1\.x, but lib is \d+\.\d+\.\d+$", e.Message);
        }
    }
}
