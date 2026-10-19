using DESpeedrunUtil.Memory;
using DESpeedrunUtil.Overlay;
using System.Diagnostics;

internal static class NativeStartupTests {
    internal static void Run(Action<bool, string> check) {
        var retry = new NativeAttachRetry();
        check(retry.CanAttempt(100), "Native HUD initially attempts attachment");
        retry.RetryLater(100);
        check(!retry.Unsupported && !retry.CanAttempt(349) && retry.CanAttempt(350), "Early startup failure schedules a retry rather than permanent desktop fallback");
        long now = 350;
        for(int i = 0; i < 20; i++) { retry.RetryLater(now); now += 2000; }
        check(retry.CanAttempt(now) && !retry.Unsupported, "A slow game startup remains recoverable after many transient failures");
        retry.RejectUnsupported();
        check(retry.Unsupported && !retry.CanAttempt(long.MaxValue), "Incompatible game code still permits the existing fallback without repeated hooks");
        retry.Reset();
        check(!retry.Unsupported && retry.CanAttempt(0), "A new game process clears the previous attachment outcome");

        using var process = Process.GetCurrentProcess();
        IntPtr memory = process.AllocateMemory(4096);
        try {
            IntPtr system = memory + 256, vtable = memory + 512, registry = memory + 768, array = memory + 1024;
            IntPtr width = memory + 32, height = memory + 36;
            bool Waiting() {
                try { NativeSpeedometer.RequireReady(process, memory, width, height); return false; }
                catch(NativeGameNotReadyException) { return true; }
            }
            check(Waiting(), "Empty command-system global is recognized as game startup");
            process.WriteBytes(memory, BitConverter.GetBytes(system.ToInt64()));
            process.WriteBytes(system, BitConverter.GetBytes(vtable.ToInt64()));
            process.WriteBytes(vtable + 24, BitConverter.GetBytes(1234L));
            check(Waiting(), "Command vtable alone is insufficient before registry initialization");
            process.WriteBytes(system + 16, BitConverter.GetBytes(registry.ToInt64()));
            process.WriteBytes(registry, BitConverter.GetBytes(array.ToInt64()));
            process.WriteBytes(registry + 8, BitConverter.GetBytes(1).Concat(BitConverter.GetBytes(16)).ToArray());
            check(Waiting(), "HUD waits for font initialization even after console startup");
            process.WriteBytes(width, BitConverter.GetBytes(float.NaN)); process.WriteBytes(height, BitConverter.GetBytes(16f));
            check(Waiting(), "Invalid initial font metrics cannot install the native renderer");
            process.WriteBytes(width, BitConverter.GetBytes(10f));
            byte[] before = process.ReadBytes(memory, 4096);
            var ready = NativeSpeedometer.RequireReady(process, memory, width, height);
            check(ready.System == system && ready.AddCommand == new IntPtr(1234), "The same staged engine becomes ready without restarting DESRU");
            check(before.SequenceEqual(process.ReadBytes(memory, 4096)), "Readiness probing does not patch or allocate in the game");
            retry.Reset();
            check(retry.CanAttempt(0), "Successful attachment clears startup backoff");
        } finally { process.FreeMemory(memory); }
    }
}
