using DESpeedrunUtil.Memory;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace DESpeedrunUtil.Overlay {
    internal static class LowHealthHudHook {
        private const string Magic = "DESRUH03";

        internal static void Attach(Process game, IntPtr state) {
            var layout = NativeGameLayout.Resolve(game.MainModule!.FileName);
            var profile = layout.Tint ?? throw new NotSupportedException(layout.TintFailure ?? "Cannot resolve low-health layout");
            int[] Sites = { profile.HudHook };
            byte[][] Originals = { Convert.FromHexString("488B43104885C0") };
            IntPtr image = game.MainModule.BaseAddress;
            var before = Sites.Select((site, i) => game.ReadBytes(image + site, Originals[i].Length) ?? Array.Empty<byte>()).ToArray();
            if(before[0].SequenceEqual(Originals[0])) { Install(game, image, state, before, profile, Sites, Originals); return; }
            if(before[0].Length != Originals[0].Length || before[0][0] != 0xE9) throw new NotSupportedException("Screen-vignette drawing was changed by another hook");
            IntPtr block = image + Sites[0] + 5 + BitConverter.ToInt32(before[0], 1);
            byte[] marker = game.ReadBytes(block + 0x300, 16) ?? Array.Empty<byte>();
            string oldMagic = marker.Length == 16 ? Encoding.ASCII.GetString(marker, 0, 8) : "";
            byte[] expected = oldMagic == "DESRUH01" ? LowHealthTintCode.CreateHudDiscovery(state.ToInt64(), (image + profile.HudVtable).ToInt64(), (image + Sites[0] + Originals[0].Length).ToInt64()) :
                LowHealthTintCode.CreateHudControl(state.ToInt64(), (image + profile.HudVtable).ToInt64(), (image + Sites[0] + Originals[0].Length).ToInt64(), profile.SwfRootOffset);
            if(!(game.ReadBytes(block, expected.Length) ?? Array.Empty<byte>()).SequenceEqual(expected) || marker.Length != 16 ||
                (oldMagic != Magic && oldMagic != "DESRUH01" && oldMagic != "DESRUH02") || BitConverter.ToInt64(marker, 8) != state.ToInt64())
                throw new NotSupportedException("HUD tint bridge does not match this session");
            if(oldMagic != Magic) Install(game, image, state, before, profile, Sites, Originals);
        }

        private static void Install(Process game, IntPtr image, IntPtr state, byte[][] before, NativeTintLayout profile, int[] Sites, byte[][] Originals) {
            IntPtr allocation = AllocateNear(game, image + Sites[0]);
            if(allocation == IntPtr.Zero) throw new InvalidOperationException("No free memory within low-health JMP range");
            bool mayBeReferenced = false;
            try {
                byte[] page = new byte[4096];
                for(int i = 0; i < Sites.Length; i++) {
                    byte[] bridge = LowHealthTintCode.CreateHudControl(state.ToInt64(), (image + profile.HudVtable).ToInt64(), (image + Sites[i] + Originals[i].Length).ToInt64(), profile.SwfRootOffset);
                    bridge.CopyTo(page, i * 0x100);
                }
                Encoding.ASCII.GetBytes(Magic).CopyTo(page, 0x300);
                BitConverter.GetBytes(state.ToInt64()).CopyTo(page, 0x308);
                if(!game.WriteBytes(allocation, page) || !game.VirtualProtect(allocation, 4096, MemPageProtect.PAGE_EXECUTE_READ) ||
                    !FlushInstructionCache(game.Handle, allocation, (UIntPtr) page.Length)) throw new InvalidOperationException("Cannot initialize low-health bridges");
                // Replace whole instructions of the same total length. With the game suspended,
                // no thread can execute a partially written JMP; all exits resume it in finally.
                if(WinAPI.NtSuspendProcess(game.Handle) != IntPtr.Zero) throw new InvalidOperationException("Cannot suspend game for low-health hook installation");
                var protection = new MemPageProtect[Sites.Length];
                int protectedSites = 0;
                bool committed = false;
                try {
                    for(int i = 0; i < Sites.Length; i++) {
                        if(!(game.ReadBytes(image + Sites[i], before[i].Length) ?? Array.Empty<byte>()).SequenceEqual(before[i]))
                            throw new InvalidOperationException("Low-health code changed while attaching");
                    }
                    for(int i = 0; i < Sites.Length; i++) {
                        if(!game.VirtualProtect(image + Sites[i], Originals[i].Length, MemPageProtect.PAGE_EXECUTE_READWRITE, out protection[i]))
                            throw new InvalidOperationException("Cannot protect low-health instruction");
                        protectedSites++;
                    }
                    mayBeReferenced = true;
                    for(int i = 0; i < Sites.Length; i++) {
                        byte[] patch = Enumerable.Repeat((byte) 0x90, Originals[i].Length).ToArray();
                        patch[0] = 0xE9;
                        long delta = (allocation + i * 0x100).ToInt64() - (image + Sites[i] + 5).ToInt64();
                        BitConverter.GetBytes(checked((int) delta)).CopyTo(patch, 1);
                        if(!game.WriteBytes(image + Sites[i], patch)) throw new InvalidOperationException("Cannot install low-health JMP");
                    }
                    if(!FlushInstructionCache(game.Handle, IntPtr.Zero, UIntPtr.Zero)) throw new InvalidOperationException("Cannot flush low-health code");
                    committed = true;
                } finally {
                    try {
                        if(!committed && mayBeReferenced) {
                            bool restored = true;
                            for(int i = 0; i < Sites.Length; i++) restored &= game.WriteBytes(image + Sites[i], before[i]);
                            restored &= FlushInstructionCache(game.Handle, IntPtr.Zero, UIntPtr.Zero);
                            if(restored) mayBeReferenced = false;
                        }
                        for(int i = 0; i < protectedSites; i++) game.VirtualProtect(image + Sites[i], Originals[i].Length, protection[i]);
                    } finally { WinAPI.NtResumeProcess(game.Handle); }
                }
            } finally {
                // Do not free any code that might still be a jump destination.
                if(!mayBeReferenced) game.FreeMemory(allocation);
            }
        }

        private static IntPtr AllocateNear(Process game, IntPtr site) {
            long low = Math.Max(0x10000, site.ToInt64() - 0x70000000), high = site.ToInt64() + 0x70000000;
            long address = low & ~0xFFFFL;
            while(address < high) {
                if(WinAPI.VirtualQueryEx(game.Handle, new IntPtr(address), out var region, (UIntPtr) Marshal.SizeOf<MemoryBasicInformation>()) == UIntPtr.Zero) break;
                long end = region.BaseAddress.ToInt64() + (long) region.RegionSize.ToUInt64();
                if(end <= address) break;
                if(region.State == MemPageState.MEM_FREE) {
                    long candidate = (Math.Max(address, region.BaseAddress.ToInt64()) + 0xFFFF) & ~0xFFFFL;
                    if(candidate + 4096 <= end && candidate + 4096 <= high) {
                        IntPtr block = WinAPI.VirtualAllocEx(game.Handle, new IntPtr(candidate), (UIntPtr) 4096,
                            (uint) (MemPageState.MEM_COMMIT | MemPageState.MEM_RESERVE), MemPageProtect.PAGE_READWRITE);
                        if(block != IntPtr.Zero) return block;
                    }
                }
                address = end;
            }
            return IntPtr.Zero;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FlushInstructionCache(IntPtr process, IntPtr address, UIntPtr size);
    }
}
