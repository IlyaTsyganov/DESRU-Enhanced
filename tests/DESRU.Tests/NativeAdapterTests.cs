using DESpeedrunUtil.Memory;
using DESpeedrunUtil.Overlay;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

internal static class NativeAdapterTests {
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate void Hud(IntPtr hud, IntPtr gui);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate float Dimension(IntPtr gui);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate void Draw(IntPtr gui, float x, float y, IntPtr text, IntPtr color, byte forceColor, float scale);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate void AddCommand(IntPtr self, IntPtr name, IntPtr callback, IntPtr help, IntPtr autocomplete, uint flags);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate void Command(IntPtr args);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate void Print(int channel, int severity, IntPtr format, IntPtr arguments);

    internal static void Run(Action<bool, string> check) {
        using var process = Process.GetCurrentProcess();
        IntPtr memory = process.AllocateMemory(8192), args = IntPtr.Zero, value = IntPtr.Zero, target = IntPtr.Zero;
        if(memory == IntPtr.Zero) throw new Exception("Cannot allocate local adapter test memory");
        var draws = new List<(IntPtr Gui, float X, float Y, string Text, float Scale, byte Force, float[] Color)>();
        var commands = new List<(string Name, IntPtr Callback, uint Flags)>();
        int originals = 0;
        var messages = new List<(int Channel, int Severity, string Format, string Text)>();
        Print print = (channel, severity, format, arguments) => messages.Add((channel, severity, Marshal.PtrToStringAnsi(format)!, Marshal.PtrToStringAnsi(Marshal.ReadIntPtr(arguments))!));
        Hud original = (_, _) => originals++;
        Dimension width = _ => 1920;
        Dimension height = _ => 1080;
        Draw draw = (gui, x, y, text, color, force, scale) => {
            var rgba = new float[4]; Marshal.Copy(color, rgba, 0, 4);
            draws.Add((gui, x, y, Marshal.PtrToStringAnsi(text)!, scale, force, rgba));
        };
        AddCommand add = (_, name, callback, _, _, flags) => commands.Add((Marshal.PtrToStringAnsi(name)!, callback, flags));
        try {
            IntPtr state = memory + 4096;
            byte[] data = NativeSpeedometer.CreateState();
            NativeSpeedometer.CreateColors(new SpeedometerReading(true, 0, 0, 0, TextColor: Color.FromArgb(255, 0, 128)),
                new SpeedometerReading(true, 0, 0, 0, TextColor: Color.Lime)).CopyTo(data, 384);
            BitConverter.GetBytes(2).CopyTo(data, 0);
            BitConverter.GetBytes(2).CopyTo(data, LowHealthTintCode.LeaseOffset);
            for(int i = 0; i < 2; i++) {
                int at = 64 + i * 64;
                BitConverter.GetBytes(1).CopyTo(data, at);
                BitConverter.GetBytes(250f).CopyTo(data, at + 4);
                BitConverter.GetBytes(i == 0 ? 500f : 750f).CopyTo(data, at + 8);
                BitConverter.GetBytes(0.5f).CopyTo(data, at + 12);
                BitConverter.GetBytes(i == 0 ? 384f : 64f).CopyTo(data, at + 48);
                Encoding.ASCII.GetBytes(i == 0 ? "12.3" : "45.6").CopyTo(data, at + 16);
            }
            process.WriteBytes(state, data);
            process.WriteBytes(memory, NativeSpeedometerCode.Render(memory.ToInt64(), state.ToInt64(),
                Marshal.GetFunctionPointerForDelegate(original).ToInt64(), Marshal.GetFunctionPointerForDelegate(draw).ToInt64(),
                Marshal.GetFunctionPointerForDelegate(width).ToInt64(), Marshal.GetFunctionPointerForDelegate(height).ToInt64(),
                123, Marshal.GetFunctionPointerForDelegate(add).ToInt64(), Marshal.GetFunctionPointerForDelegate(print).ToInt64()));
            for(int i = 0; i < NativeSpeedometerCode.Commands.Length; i++) process.WriteBytes(memory + NativeSpeedometerCode.CallbackStart + i * NativeSpeedometerCode.CallbackStride,
                NativeSpeedometerCode.Command((state + 0x400 + i * 0x80).ToInt64(), i == 6 ? 4 : 2));
            var render = Marshal.GetDelegateForFunctionPointer<Hud>(memory);
            render(new IntPtr(100), new IntPtr(200));
            check(originals == 1, "Native adapter preserves original HUD render call");
            check(commands.Count == 7 && commands.Select(c => c.Name).SequenceEqual(NativeSpeedometerCode.Commands), "Seven console commands include the renamed show command, landing flag, RGB, and low-health tint");
            check(process.ReadValue<int>(state + LowHealthTintCode.LeaseOffset) == 1, "HUD advances the independent tint heartbeat");
            check(commands.All(c => c.Flags == 2), "Console commands allow use without cheats");
            check(draws.Count == 2 && draws[0].Text == "12.3" && draws[1].Text == "45.6", "Native renderer receives only numeric strings");
            check(draws.All(d => d.Gui == new IntPtr(200) && d.Scale == 0.5f && d.Force == 1), "Native text ABI preserves model, float scale and color arguments");
            check(draws[0].Color.SequenceEqual(new[] { 1f, 0f, 128 / 255f, 1f }) && draws[1].Color.SequenceEqual(new[] { 0f, 1f, 0f, 1f }), "Each native DrawString receives its own normalized RGB color with opaque alpha");
            check(draws[0].X == 250 && draws[0].Y == 500 && draws[1].Y == 750,
                "Native coordinates use physical pixels from the top-left corner");
            process.WriteBytes(state + 56, BitConverter.GetBytes(3840f));
            process.WriteBytes(state + 60, BitConverter.GetBytes(2160f));
            render(new IntPtr(100), new IntPtr(200));
            check(commands.Count == 7 && originals == 2, "Commands register once");
            check(draws[2].X == 125 && draws[2].Y == 250, "4K physical pixels convert to virtual HUD units without changing their physical location");
            int before = draws.Count;
            render(new IntPtr(100), new IntPtr(200));
            check(draws.Count == before && originals == 3, "Expired heartbeat stops drawing and retains original HUD");
            process.WriteBytes(state + 4, BitConverter.GetBytes(1));
            process.WriteBytes(state + LowHealthTintCode.LeaseOffset, BitConverter.GetBytes(2));
            render(new IntPtr(100), new IntPtr(200));
            check(commands.Count == 10 && commands.TakeLast(3).Select(x => x.Name).SequenceEqual(NativeSpeedometerCode.Commands.Skip(4)), "Legacy four-command state registers only tint, landing flag and color");
            check(process.ReadValue<int>(state + LowHealthTintCode.LeaseOffset) == 1, "Tint heartbeat remains active with both speedometers disabled");
            process.WriteBytes(state + 4, BitConverter.GetBytes(2));
            render(new IntPtr(100), new IntPtr(200));
            check(commands.Count == 12 && commands.TakeLast(2).Select(x => x.Name).SequenceEqual(NativeSpeedometerCode.Commands.Skip(5)), "Five-command state registers only landing flag and color");
            process.WriteBytes(state, BitConverter.GetBytes(1));
            process.WriteBytes(state + 68, BitConverter.GetBytes(5000f));
            process.WriteBytes(state + 72, BitConverter.GetBytes(5000f));
            process.WriteBytes(state + 132, BitConverter.GetBytes(5000f));
            process.WriteBytes(state + 136, BitConverter.GetBytes(5000f));
            render(new IntPtr(100), new IntPtr(200));
            check(draws[before].X == 1728 && draws[before].Y == 1056, "Oversized pixel coordinates clamp the complete text inside the virtual viewport");
            check(draws[before + 1].X == 1888 && draws[before + 1].Y == 1056, "Different text widths clamp independently at the right edge");
            args = Marshal.AllocHGlobal(1032);
            value = Marshal.StringToHGlobalAnsi("37"); target = Marshal.StringToHGlobalAnsi("landing");
            Marshal.WriteInt32(args, 3); Marshal.WriteIntPtr(args, 16, value); Marshal.WriteIntPtr(args, 24, target);
            var callback = Marshal.GetDelegateForFunctionPointer<Command>(commands[1].Callback);
            callback(args);
            var mail = process.ReadBytes(state + 0x480, 80);
            check(BitConverter.ToInt32(mail, 0) == 1 && BitConverter.ToInt32(mail, 4) == 3 && Encoding.ASCII.GetString(mail, 16, 2) == "37" && Encoding.ASCII.GetString(mail, 48, 7) == "landing",
                "Native console callback captures bounded value and optional target");
            Marshal.WriteInt32(args, 1); callback(args);
            mail = process.ReadBytes(state + 0x480, 80);
            check(mail[16] == 0 && mail[48] == 0, "Missing arguments cannot reuse a previous command value");
            var colorArgs = new[] { "255", "128", "0", "landing" }.Select(Marshal.StringToHGlobalAnsi).ToArray();
            try {
                Marshal.WriteInt32(args, 5);
                for(int i = 0; i < 4; i++) Marshal.WriteIntPtr(args, 16 + i * 8, colorArgs[i]);
                var colorCallback = Marshal.GetDelegateForFunctionPointer<Command>(commands[6].Callback);
                colorCallback(args);
                mail = process.ReadBytes(state + 0x700, 144);
                check(Enumerable.Range(0, 4).All(i => Encoding.ASCII.GetString(mail, 16 + i * 32, new[] { 3, 3, 1, 7 }[i]) == new[] { "255", "128", "0", "landing" }[i]), "RGB callback captures all three channels and optional target without crossing the string block");
                Marshal.WriteInt32(args, 1); colorCallback(args); mail = process.ReadBytes(state + 0x700, 144);
                check(Enumerable.Range(0, 4).All(i => mail[16 + i * 32] == 0), "Short RGB requests clear every previous argument");
            } finally { foreach(var p in colorArgs) Marshal.FreeHGlobal(p); }
            process.WriteBytes(state, new byte[4]);
            process.WriteBytes(state + NativeSpeedometerCode.ConsoleTextOffset, Encoding.ASCII.GetBytes("help with literal %n and 100%\n\0"));
            process.WriteBytes(state + NativeSpeedometerCode.ConsoleReadyOffset, BitConverter.GetBytes(1));
            int beforeHelp = originals;
            render(new IntPtr(100), new IntPtr(200));
            check(messages.Count == 1 && messages[0] == (1, 1, "%s", "help with literal %n and 100%\n"), "Console response uses the verified VPrintf ABI and a constant format string");
            check(originals == beforeHelp + 1 && process.ReadValue<int>(state + NativeSpeedometerCode.ConsoleReadyOffset) == 0, "Help works while speedometers are hidden, preserves original HUD, and acknowledges the buffer");
            render(new IntPtr(100), new IntPtr(200));
            check(messages.Count == 1, "Console response is printed exactly once per publication");
        } finally {
            if(args != IntPtr.Zero) Marshal.FreeHGlobal(args);
            if(value != IntPtr.Zero) Marshal.FreeHGlobal(value);
            if(target != IntPtr.Zero) Marshal.FreeHGlobal(target);
            process.FreeMemory(memory);
            GC.KeepAlive(original); GC.KeepAlive(width); GC.KeepAlive(height); GC.KeepAlive(draw); GC.KeepAlive(add);
            GC.KeepAlive(print);
        }
    }
}
