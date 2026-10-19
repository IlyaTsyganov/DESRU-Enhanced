using DESpeedrunUtil.Memory;
using DESpeedrunUtil.Overlay;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Settings = DESpeedrunUtil.Properties.Settings;

internal static class LowHealthTintTests {
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate float Normalize(float alpha);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate void PostProcess(IntPtr view, IntPtr output);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate float Vignette(IntPtr view, IntPtr layer, IntPtr output);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate void Hud(IntPtr element);

    internal static void Run(Action<bool, string> check) {
        var settings = new Settings();
        check(settings.LowHpTintOpacity == 100, "Low-health tint defaults to original opacity");
        check(SpeedometerCommands.Apply(settings, 4, "0", "") && settings.LowHpTintOpacity == 0, "Tint zero is accepted");
        check(SpeedometerCommands.Apply(settings, 4, "37.5", "") && settings.LowHpTintOpacity == 37.5, "Fractional tint percentages are accepted");
        check(SpeedometerCommands.Apply(settings, 4, "100", "") && settings.LowHpTintOpacity == 100, "Tint 100 restores the original setting");
        check(!SpeedometerCommands.Apply(settings, 4, "-1", "") && !SpeedometerCommands.Apply(settings, 4, "101", "") &&
            !SpeedometerCommands.Apply(settings, 4, "NaN", "") && !SpeedometerCommands.Apply(settings, 4, "50", "landing"), "Invalid tint values/targets are rejected");

        using var process = Process.GetCurrentProcess();
        IntPtr memory = process.AllocateMemory(65536);
        if(memory == IntPtr.Zero) throw new Exception("Cannot allocate tint adapter test");
        try {
            IntPtr state = memory + 4096;
            process.WriteBytes(state + 256, BitConverter.GetBytes(1f / 255));
            process.WriteBytes(state + 260, BitConverter.GetBytes(255f));
            process.WriteBytes(memory + 0x300, new byte[] { 0xC3 });
            foreach(bool combined in new[] { false, true }) {
                process.WriteBytes(memory + 0x100, LowHealthTintCode.Create(state.ToInt64(), (memory + 0x300).ToInt64(), (state + 256).ToInt64(), combined));
                // Normal branch's original instruction reads XMM10. Preserve that nonvolatile
                // register around a controlled call, exactly as the game's enclosing function does.
                var harness = new List<byte>();
                void Emit(string hex) => harness.AddRange(Convert.FromHexString(hex));
                void Pointer(long value) => harness.AddRange(BitConverter.GetBytes(value));
                Emit("4883EC38 440F11542420".Replace(" ", ""));
                Emit("48B8"); Pointer((state + 260).ToInt64()); Emit("F3440F1010");
                Emit("48B8"); Pointer((memory + 0x100).ToInt64()); Emit("FFD0");
                Emit("440F105424204883C438C3");
                process.WriteBytes(memory, harness.ToArray());
                var normalize = Marshal.GetDelegateForFunctionPointer<Normalize>(memory);
                process.WriteBytes(state + LowHealthTintCode.LeaseOffset, BitConverter.GetBytes(240));
                process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(1f));
                float original = normalize(204);
                check(Math.Abs(original - 0.8f) < 0.000001, $"Original alpha normalization is retained (combined={combined})");
                process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(0.5f));
                check(Math.Abs(normalize(204) - 0.4f) < 0.000001, $"50 percent halves the original alpha (combined={combined})");
                process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(0f));
                check(normalize(204) == 0, $"Zero removes the low-health alpha (combined={combined})");
                process.WriteBytes(state + LowHealthTintCode.LeaseOffset, BitConverter.GetBytes(0));
                check(normalize(204) == original, $"Expired lease restores exact original alpha (combined={combined})");
            }
            IntPtr view = memory + 8192, output = memory + 0x7000;
            var originalVector = new[] { 0.2f, 0.8f, 0.3f, 0.4f };
            byte[] vector = originalVector.SelectMany(BitConverter.GetBytes).ToArray();
            process.WriteBytes(view + 0x3588, vector);
            process.WriteBytes(memory + 0x300, Convert.FromHexString("0F1102C3")); // save XMM0 to output, return
            process.WriteBytes(memory + 0x100, LowHealthTintCode.CreatePostProcess(state.ToInt64(), (memory + 0x300).ToInt64()));
            var call = new List<byte>(Convert.FromHexString("564883EC20488BF148B8"));
            call.AddRange(BitConverter.GetBytes((memory + 0x100).ToInt64()));
            call.AddRange(Convert.FromHexString("FFD04883C4205EC3"));
            process.WriteBytes(memory, call.ToArray());
            var postProcess = Marshal.GetDelegateForFunctionPointer<PostProcess>(memory);
            process.WriteBytes(state + LowHealthTintCode.LeaseOffset, BitConverter.GetBytes(240));
            foreach(float scale in new[] { 0f, 0.5f, 1f }) {
                process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(scale));
                postProcess(view, output);
                byte[] result = process.ReadBytes(output, 16);
                check(BitConverter.ToSingle(result, 4) == 0.8f * scale && BitConverter.ToSingle(result, 0) == 0.2f &&
                    BitConverter.ToSingle(result, 8) == 0.3f && BitConverter.ToSingle(result, 12) == 0.4f,
                    $"Post-process scale {scale} changes only the low-health component");
            }
            process.WriteBytes(state + LowHealthTintCode.LeaseOffset, BitConverter.GetBytes(0));
            process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(0f));
            postProcess(view, output);
            check(process.ReadBytes(output, 16).SequenceEqual(vector), "Expired post-process lease returns the original vector");
            check(process.ReadBytes(view + 0x3588, 16).SequenceEqual(vector), "Post-process bridge never changes the game's cached player-state vector");

            IntPtr layer = view + 0x3080;
            process.WriteBytes(memory + 0x300, new byte[] { 0xC3 });
            process.WriteBytes(memory + 0x100, LowHealthTintCode.CreateVignette(state.ToInt64(), (memory + 0x300).ToInt64()));
            var vignetteCall = new List<byte>(Convert.FromHexString("575341564883EC20488BF9488BDA4D8BF048B8"));
            vignetteCall.AddRange(BitConverter.GetBytes((memory + 0x100).ToInt64()));
            vignetteCall.AddRange(Convert.FromHexString("FFD04883C420415E5B5FC3"));
            process.WriteBytes(memory, vignetteCall.ToArray());
            var vignette = Marshal.GetDelegateForFunctionPointer<Vignette>(memory);
            process.WriteBytes(state + 232, BitConverter.GetBytes(123L));
            process.WriteBytes(state + 240, BitConverter.GetBytes(1)); process.WriteBytes(state + 244, BitConverter.GetBytes(2));
            process.WriteBytes(view + 0x2DF8, BitConverter.GetBytes(123L));
            process.WriteBytes(layer, BitConverter.GetBytes(0));
            process.WriteBytes(state + LowHealthTintCode.LeaseOffset, BitConverter.GetBytes(240));
            foreach(int action in new[] { 1, 2 }) {
                process.WriteBytes(view + 0x2E00, BitConverter.GetBytes(action));
                foreach(float scale in new[] { 0f, 0.5f, 1f }) {
                    process.WriteBytes(output + 4, vector);
                    process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(scale));
                    float bookkeepingAlpha = vignette(view, layer + 0x3C, output);
                    check(process.ReadValue<float>(output + 16) == 0.4f * scale && process.ReadValue<float>(output + 4) == 0.2f && process.ReadValue<float>(output + 8) == 0.8f,
                        $"Vignette action {action} scales final alpha at {scale} without changing RGB");
                    check(bookkeepingAlpha == 0.4f, "Transparent vignette retains the original alpha for the game's record-count branch");
                }
            }
            process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(0f));
            foreach((int index, long group, int action, int lease) in new[] { (-1, 123L, 1, 240), (8, 123L, 1, 240), (0, 999L, 1, 240), (0, 123L, 8, 240), (0, 123L, 1, 0) }) {
                process.WriteBytes(layer, BitConverter.GetBytes(index));
                process.WriteBytes(view + 0x2DF8, BitConverter.GetBytes(group));
                process.WriteBytes(view + 0x2E00, BitConverter.GetBytes(action));
                process.WriteBytes(state + LowHealthTintCode.LeaseOffset, BitConverter.GetBytes(lease));
                process.WriteBytes(output + 4, vector);
                vignette(view, layer + 0x3C, output);
                check(process.ReadBytes(output + 4, 16).SequenceEqual(vector), $"Vignette excludes invalid index, other FX/action or expired lease ({index},{group},{action},{lease})");
            }
            process.WriteBytes(memory + 0x100, LowHealthTintCode.CreateHudDiscovery(state.ToInt64(), 123, (memory + 0x300).ToInt64()));
            var hudCall = new List<byte>(Convert.FromHexString("534883EC20488BD948B8"));
            hudCall.AddRange(BitConverter.GetBytes((memory + 0x100).ToInt64()));
            hudCall.AddRange(Convert.FromHexString("FFD04883C4205BC3"));
            process.WriteBytes(memory, hudCall.ToArray());
            var hud = Marshal.GetDelegateForFunctionPointer<Hud>(memory);
            process.WriteBytes(view, BitConverter.GetBytes(123L)); process.WriteBytes(view + 16, BitConverter.GetBytes(456L));
            hud(view);
            check(process.ReadPointer(state + 280) == view && process.ReadValue<long>(state + 288) == 456 && process.ReadValue<long>(state + 304) == 1, "HUD discovery captures only the verified low-warning element");
            process.WriteBytes(view, BitConverter.GetBytes(999L)); process.WriteBytes(view + 16, BitConverter.GetBytes(789L));
            hud(view);
            check(process.ReadValue<long>(state + 288) == 456 && process.ReadValue<long>(state + 304) == 1, "Other HUD elements never replace the low-warning target");

            IntPtr swf = memory + 0x8000, root = memory + 0x9000, lowHealth = memory + 0xA000, background = memory + 0xB000;
            IntPtr rootList = memory + 0xC000, lowList = memory + 0xC100, lowName = memory + 0xD000, bgName = memory + 0xD020;
            void Ptr(IntPtr address, IntPtr value) => process.WriteBytes(address, BitConverter.GetBytes(value.ToInt64()));
            process.WriteBytes(view, BitConverter.GetBytes(123L)); Ptr(view + 16, swf); Ptr(swf + 0x4D0, root);
            Ptr(root + 0x70, rootList); process.WriteBytes(root + 0x78, BitConverter.GetBytes(1)); Ptr(rootList + 0x30, lowHealth);
            Ptr(lowHealth + 0x18, lowName); process.WriteBytes(lowName, System.Text.Encoding.ASCII.GetBytes("lowHealth\0"));
            Ptr(lowHealth + 0x70, lowList); process.WriteBytes(lowHealth + 0x78, BitConverter.GetBytes(1)); Ptr(lowList + 0x30, background);
            Ptr(background + 0x18, bgName); process.WriteBytes(bgName, System.Text.Encoding.ASCII.GetBytes("backgroundFx\0"));
            process.WriteBytes(lowList + 0x18, BitConverter.GetBytes(0.8f)); process.WriteBytes(lowList + 0x28, BitConverter.GetBytes(0.1f));
            process.WriteBytes(lowList + 0x0C, BitConverter.GetBytes(0.7f)); // RGB multiplier must stay intact.
            process.WriteBytes(memory + 0x100, LowHealthTintCode.CreateHudControl(state.ToInt64(), 123, (memory + 0x3000).ToInt64()));
            // The larger bridge needs a continuation outside its own code span.
            process.WriteBytes(memory + 0x3000, new byte[] { 0xC3 });
            process.WriteBytes(state + LowHealthTintCode.LeaseOffset, BitConverter.GetBytes(240));
            foreach(float scale in new[] { 0f, 0.5f, 0.5f, 1f, 0f, 1f }) {
                process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(scale));
                hud(view);
                check(process.ReadValue<float>(lowList + 0x18) == 0.8f * scale && process.ReadValue<float>(lowList + 0x28) == 0.1f * scale && process.ReadValue<float>(lowList + 0x0C) == 0.7f,
                    $"HUD low-health background scales both alpha terms at {scale} without accumulation or RGB changes");
            }
            process.WriteBytes(lowList + 0x18, BitConverter.GetBytes(0.6f)); process.WriteBytes(lowList + 0x28, BitConverter.GetBytes(0.2f));
            process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(0.5f)); hud(view);
            check(process.ReadValue<float>(lowList + 0x18) == 0.3f && process.ReadValue<float>(lowList + 0x28) == 0.1f, "A new game-side alpha value is retained as the new unscaled baseline");
            process.WriteBytes(state + LowHealthTintCode.LeaseOffset, BitConverter.GetBytes(0)); hud(view);
            check(process.ReadValue<float>(lowList + 0x18) == 0.6f && process.ReadValue<float>(lowList + 0x28) == 0.2f, "Expired HUD heartbeat restores the captured original alpha");
            process.WriteBytes(state + LowHealthTintCode.LeaseOffset, BitConverter.GetBytes(240));
            process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(0f));
            process.WriteBytes(view, BitConverter.GetBytes(999L)); hud(view);
            check(process.ReadValue<float>(lowList + 0x18) == 0.6f, "Other HUD classes remain untouched by the tint bridge");
            process.WriteBytes(view, BitConverter.GetBytes(123L));
            Ptr(swf + 0x4D0, IntPtr.Zero); Ptr(swf + 0x18, root);
            process.WriteBytes(memory + 0x100, LowHealthTintCode.CreateHudControl(state.ToInt64(), 123, (memory + 0x3000).ToInt64(), 0x18));
            hud(view);
            check(process.ReadValue<float>(lowList + 0x18) == 0 && process.ReadValue<float>(lowList + 0x28) == 0, "Modern SWF root offset is used instead of the legacy root pointer");

            IntPtr modernLayer = view + 0x3120, registerReport = memory + 0xE000;
            process.WriteBytes(memory + 0x100, LowHealthTintCode.CreateIndexedVignette(state.ToInt64(), (memory + 0x100).ToInt64(), (memory + 0x3000).ToInt64(), 0x2E98, 0x8C0));
            var modernCall = new List<byte>(Convert.FromHexString("5657415641574883EC28488BF1488BFA4D8BF041BF0300000048B8"));
            modernCall.AddRange(BitConverter.GetBytes((memory + 0x100).ToInt64()));
            modernCall.AddRange(Convert.FromHexString("FFD048B8")); modernCall.AddRange(BitConverter.GetBytes(registerReport.ToInt64()));
            modernCall.AddRange(Convert.FromHexString("4889384C8970084C8978104883C428415F415E5F5EC3"));
            process.WriteBytes(memory, modernCall.ToArray());
            var modernVignette = Marshal.GetDelegateForFunctionPointer<Vignette>(memory);
            process.WriteBytes(state + 232, BitConverter.GetBytes(123L));
            process.WriteBytes(state + 240, BitConverter.GetBytes(1)); process.WriteBytes(state + 244, BitConverter.GetBytes(2));
            process.WriteBytes(view + 0x2E98, BitConverter.GetBytes(123L)); process.WriteBytes(view + 0x2EA0, BitConverter.GetBytes(2));
            process.WriteBytes(modernLayer, BitConverter.GetBytes(0));
            foreach(float scale in new[] { 0f, 0.5f, 1f }) {
                process.WriteBytes(state + LowHealthTintCode.ScaleOffset, BitConverter.GetBytes(scale));
                process.WriteBytes(output + 0x8C0 + 12, BitConverter.GetBytes(0.8f));
                float bookkeeping = modernVignette(view, modernLayer + 0x3C, output);
                check(process.ReadValue<float>(output + 0x8C0 + 12) == 0.8f * scale && bookkeeping == 0.8f, $"Modern indexed output scales at {scale} and preserves game bookkeeping");
                check(process.ReadPointer(registerReport) == modernLayer + 0x3C && process.ReadPointer(registerReport + 8) == output && process.ReadValue<long>(registerReport + 16) == 3, "Indexed bridge restores layer, output base, and output index registers");
            }
            process.WriteBytes(state + LowHealthTintCode.LeaseOffset, new byte[4]);
            process.WriteBytes(output + 0x8C0 + 12, BitConverter.GetBytes(0.8f));
            modernVignette(view, modernLayer + 0x3C, output);
            check(process.ReadValue<float>(output + 0x8C0 + 12) == 0.8f, "Expired lease leaves modern screen FX unmodified");
        } finally { process.FreeMemory(memory); }
    }
}
