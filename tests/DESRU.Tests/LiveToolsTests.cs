using DESpeedrunUtil.Memory;
using DESpeedrunUtil.Overlay;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

internal static class LiveToolsTests {
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate void Render(IntPtr state, IntPtr gui);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate float Dimension(IntPtr gui);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate void Draw(IntPtr gui, float x, float y, IntPtr text, IntPtr color, byte force, float scale);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate long Action(IntPtr first, IntPtr second);

    internal static void Run(Action<bool, string> check) {
        check(ProtectedCvarMonitor.IsDefault("0", "false", 1) && ProtectedCvarMonitor.IsDefault("1", "true", 1), "Boolean cvar defaults accept engine spellings");
        check(ProtectedCvarMonitor.IsDefault("0.500000", "0.5f", 4) && ProtectedCvarMonitor.IsDefault("20.307301", "20.3073", 4), "Cvar float defaults compare at game float precision");
        check(!ProtectedCvarMonitor.IsDefault("0.5", "1.0f", 4) && !ProtectedCvarMonitor.IsDefault("A", "a", 0), "Changed numeric and case-sensitive string cvars remain visible");
        var baseline = new[] { new CvarDifference("s_volume", "0.06", "1", 12), new CvarDifference("view_skipShakes", "1", "0", 9), new CvarDifference("unchanged", "2", "1", 12) };
        var restored = CheatCvarSession.Restores(baseline, new[] { new CvarDifference("s_volume", "0.2", "1", 12), new CvarDifference("unchanged", "2.000", "1", 12), new CvarDifference("timescale", "0.5", "1", 12) });
        check(restored.Contains(new CvarRestore("s_volume", "0.06")), "Settings changed during cheats restore to the user's original value");
        check(restored.Contains(new CvarRestore("view_skipShakes", "1")), "A user setting changed to factory default still restores to its original value");
        check(restored.Contains(new CvarRestore("timescale", null)), "New cheat changes restore to factory default");
        check(restored.Count == 3 && !restored.Any(r => r.Name == "unchanged"), "Unchanged pre-existing preferences are never reset");
        check(CheatCvarSession.Restores(baseline, baseline).Count == 0, "Turning off an unchanged cheat session preserves all initial preferences");
        var preferences = new[] { new CvarDifference("r_gamma", "1.5", "1.2", 12), new CvarDifference("s_volume", "0.06", "1", 12), new CvarDifference("g_weaponAutoSwitchOnEmpty", "0", "1", 9) };
        var displayed = CheatCvarSession.DisplayChanges(preferences, preferences.Concat(new[] { new CvarDifference("timescale", "0.5", "1", 12) }).ToArray());
        check(displayed.Count == 1 && displayed[0].Name == "timescale", "Cvar HUD hides preserved gamma, volume and weapon preferences while showing a new cheat change");
        check(CheatCvarSession.DisplayChanges(preferences, new[] { new CvarDifference("S_VOLUME", "0.2", "1", 12) }).Count == 0, "Preserved preference names stay hidden even if changed again while cheats are enabled");
        check(CheatCvarSession.DisplayChanges(preferences, preferences).Count == 0, "Initial nondefault settings produce an empty cheat HUD");
        var noclipFlag = NativePlayerCheatFlag.Decode(Convert.FromHexString("0FB68196660000C1E80283E001C3CCCC"));
        var godFlag = NativePlayerCheatFlag.Decode(Convert.FromHexString("0FB681107C0300D1E883E001C3CCCCCC"));
        check(noclipFlag == new NativePlayerCheatFlag(0x6696, 4) && godFlag == new NativePlayerCheatFlag(0x37c10, 2), "Reflected player getters resolve bit offsets and both shift encodings");
        bool unknownGetter = false; try { NativePlayerCheatFlag.Decode(new byte[16]); } catch(NotSupportedException) { unknownGetter = true; }
        check(unknownGetter, "Unknown player getters are rejected instead of guessing state offsets");
        var anchor = PerformanceMetricsAnchor.BelowModifiers(new Rectangle(0, 0, 1920, 1080), 1920, 1080, 16, false);
        var highDpiAnchor = PerformanceMetricsAnchor.BelowModifiers(new Rectangle(0, 0, 3840, 2160), 1920, 1080, 16, false);
        check(anchor.Top == 64 && anchor.Right == 1910 && anchor.Scale < 1.25f && highDpiAnchor.Top == anchor.Top * 2 && highDpiAnchor.Right == anchor.Right * 2,
            "Cheat rows anchor below the three DESRU modifier rows with smaller text and consistent DPI scaling");
        check(PerformanceMetricsAnchor.BelowModifiers(new Rectangle(0, 0, 1920, 1080), 1920, 1080, 32, false).Top > anchor.Top &&
            PerformanceMetricsAnchor.BelowModifiers(new Rectangle(0, 0, 1920, 1080), 1920, 1080, 16, true).Top < anchor.Top,
            "Cheat anchor follows OSD font size and minimal mode instead of speedometer positions");
        var layout = new NativeCvarLayout(0, 104, 0, 8, 12, 40, 48, 64, 8, 16, 0x8000, 0x4000, 0x400000);
        check(ProtectedCvarMonitor.IsProtected(12, layout) && !ProtectedCvarMonitor.IsProtected(0x400004, layout), "Cvar filter includes writable cheats and excludes commands compiled out of shipping builds");
        check(!ProtectedCvarMonitor.IsProtected(28, layout) && !ProtectedCvarMonitor.IsProtected(0x8008, layout) && !ProtectedCvarMonitor.IsProtected(0x4008, layout), "Ordinary, read-only and init-only cvars are excluded");
        bool rejected = false; try { LiveMeathook.PrepareImage(new byte[512]); } catch(NotSupportedException) { rejected = true; }
        check(rejected, "Unknown Meathook binaries cannot enter the live initialization path");
        string bundled = Path.Combine("output", "meath00k", "XINPUT1_3.dll");
        if(File.Exists(bundled)) {
            byte[] original = File.ReadAllBytes(bundled), copy = (byte[]) original.Clone(), patched = LiveMeathook.PrepareImage(original);
            check(original.SequenceEqual(copy) && patched.Length == original.Length && !patched.SequenceEqual(original), "Late-entry preparation preserves the original bundled DLL");
        }
        using var process = Process.GetCurrentProcess();
        IntPtr allocation = process.AllocateMemory(300000), state = allocation + 4096, bank = allocation + 8192;
        var draws = new List<(float X, float Y, string Text, float Scale)>();
        int actions = 0;
        Dimension width = _ => 1920, height = _ => 1080;
        Draw draw = (_, x, y, text, _, _, scale) => draws.Add((x, y, Marshal.PtrToStringAnsi(text)!, scale));
        Action action = (first, second) => { check(first.ToInt64() == 123 && second.ToInt64() == 456, "Game actions receive both arguments"); actions++; return 0x123456789L; };
        try {
            var flags = new PlayerCheatMonitor(process, new(new(1, 4), new(2, 32), new(2, 2)));
            IntPtr player = allocation + 290000;
            for(int bits = 0; bits < 8; bits++) {
                process.WriteBytes(player, new byte[] { 0, (byte) ((bits & 1) != 0 ? 4 : 0), (byte) (((bits & 2) != 0 ? 32 : 0) | ((bits & 4) != 0 ? 2 : 0)) });
                var expected = new PlayerCheatState((bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0);
                check(flags.Read(player) == expected, $"Player cheat flags correctly track independent on/off combination {bits}");
            }
            check(flags.Read(IntPtr.Zero) == default, "Missing player clears all command indicators");
            var cvarRows = new[] { new CvarDifference("timescale", "0.5", "1", 12) };
            var combined = new PlayerCheatState(true, false, true).AddTo(cvarRows);
            check(combined.Select(c => c.Name).SequenceEqual(new[] { "noclip", "god", "timescale" }) && combined[0].Value == "ON" &&
                ReferenceEquals(new PlayerCheatState().AddTo(cvarRows), cvarRows), "Enabled command states share the cvar list and disappear individually when off");
            byte[] code = NativeHudExtension.CreateCode(Marshal.GetFunctionPointerForDelegate(draw), Marshal.GetFunctionPointerForDelegate(width), Marshal.GetFunctionPointerForDelegate(height));
            process.WriteBytes(allocation, code); process.VirtualProtect(allocation, 4096, MemPageProtect.PAGE_EXECUTE_READ);
            process.WriteBytes(state + 8, BitConverter.GetBytes(bank.ToInt64()));
            process.WriteBytes(state + 24, BitConverter.GetBytes(1));
            process.WriteBytes(state + 80, BitConverter.GetBytes(3840f)); process.WriteBytes(state + 84, BitConverter.GetBytes(2160f));
            process.WriteBytes(bank, BitConverter.GetBytes(1));
            process.WriteBytes(bank + 16, BitConverter.GetBytes(3800f)); process.WriteBytes(bank + 20, BitConverter.GetBytes(1400f));
            process.WriteBytes(bank + 24, BitConverter.GetBytes(0.5f)); process.WriteBytes(bank + 28, BitConverter.GetBytes(100f));
            process.WriteBytes(bank + 32, Encoding.ASCII.GetBytes("timescale = 0.5\0"));
            var render = Marshal.GetDelegateForFunctionPointer<Render>(allocation);
            render(state, IntPtr.Zero);
            check(draws.Count == 1 && draws[0] == (1800f, 700f, "timescale = 0.5", 0.5f), "Protected cvar HUD uses smaller text, physical coordinates and right alignment");
            render(state, IntPtr.Zero);
            check(draws.Count == 1, "Expired cvar HUD lease hides all rows");
            process.WriteBytes(state + 40, BitConverter.GetBytes(Marshal.GetFunctionPointerForDelegate(action).ToInt64()));
            process.WriteBytes(state + 48, BitConverter.GetBytes(123L)); process.WriteBytes(state + 56, BitConverter.GetBytes(456L));
            process.WriteBytes(state + 32, BitConverter.GetBytes(1));
            render(state, IntPtr.Zero); render(state, IntPtr.Zero);
            check(actions == 1 && process.ReadValue<int>(state + 32) == 3 && process.ReadValue<long>(state + 120) == 0x123456789L, "Game-thread actions acknowledge once and preserve 64-bit results even while HUD is hidden");
            IntPtr owner = allocation + 260000;
            process.WriteBytes(owner + NativeHudExtension.PointerOffset, BitConverter.GetBytes(state.ToInt64()));
            process.WriteBytes(state + 16, Encoding.ASCII.GetBytes("DESRUE01"));
            using var extension = new NativeHudExtension(process, owner, new NativeGameLayout(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
            draws.Clear();
            extension.Update(new PlayerCheatState(true, false, true).AddTo(Array.Empty<CvarDifference>()), new Rectangle(0, 0, 3840, 2160), 128, 0.65f, 10, 16, true, true, 3820);
            render(state, IntPtr.Zero);
            check(draws.Count == 2 && draws[0].Text == "noclip = ON" && draws[1].Text == "god = ON" && draws[0].Y == 64 && draws[1].Y > draws[0].Y && draws[0].Scale == 0.65f,
                "Native cheat rows draw downward below modifiers using the reduced font size");
        } finally {
            process.FreeMemory(allocation); GC.KeepAlive(width); GC.KeepAlive(height); GC.KeepAlive(draw); GC.KeepAlive(action);
        }
    }
}
