using DESpeedrunUtil.Memory;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace DESpeedrunUtil.Overlay {
    // Remove only this session's verified experimental bridges from earlier test builds.
    // Their allocations stay alive until game exit in case a thread was inside a bridge.
    internal static class LegacyTintCleanup {
        internal static void Restore(Process game, IntPtr state) {
            int[] sites = { 0x1756076, 0x175614A, 0x17501BD };
            byte[][] originals = { Convert.FromHexString("F30F5905C221E800"), Convert.FromHexString("F3410F5EC2"), Convert.FromHexString("0F108688350000") };
            IntPtr image = game.MainModule!.BaseAddress;
            byte[] first = game.ReadBytes(image + sites[0], 8) ?? Array.Empty<byte>();
            if(first.Length != 8 || first[0] != 0xE9) return;
            IntPtr block = image + sites[0] + 5 + BitConverter.ToInt32(first, 1);
            byte[] marker = game.ReadBytes(block + 0x300, 16) ?? Array.Empty<byte>();
            if(marker.Length != 16) return;
            string magic = Encoding.ASCII.GetString(marker, 0, 8);
            if(magic != "DESRUT01" && magic != "DESRUT02" && magic != "DESRUT03") return;
            if(BitConverter.ToInt64(marker, 8) != state.ToInt64()) throw new NotSupportedException("Legacy tint belongs to another HUD state");
            int count = magic == "DESRUT03" ? 3 : 2;
            var before = new byte[count][];
            for(int i = 0; i < count; i++) {
                before[i] = game.ReadBytes(image + sites[i], originals[i].Length) ?? Array.Empty<byte>();
                if(before[i].Length != originals[i].Length || before[i][0] != 0xE9 || image + sites[i] + 5 + BitConverter.ToInt32(before[i], 1) != block + i * 0x100)
                    throw new NotSupportedException("Legacy tint jump signature differs");
                byte[] expected = i == 2 ? LowHealthTintCode.CreatePostProcess(state.ToInt64(), (image + sites[i] + originals[i].Length).ToInt64()) :
                    LowHealthTintCode.Create(state.ToInt64(), (image + sites[i] + originals[i].Length).ToInt64(), (image + 0x25D8240).ToInt64(), i == 0, magic != "DESRUT01");
                if(!(game.ReadBytes(block + i * 0x100, expected.Length) ?? Array.Empty<byte>()).SequenceEqual(expected))
                    throw new NotSupportedException("Legacy tint bridge signature differs");
            }
            if(WinAPI.NtSuspendProcess(game.Handle) != IntPtr.Zero) throw new InvalidOperationException("Cannot suspend game for legacy tint cleanup");
            var protections = new MemPageProtect[count];
            int protectedCount = 0;
            bool complete = false;
            try {
                for(int i = 0; i < count; i++) {
                    if(!(game.ReadBytes(image + sites[i], before[i].Length) ?? Array.Empty<byte>()).SequenceEqual(before[i]))
                        throw new InvalidOperationException("Legacy tint changed while restoring");
                    if(!game.VirtualProtect(image + sites[i], originals[i].Length, MemPageProtect.PAGE_EXECUTE_READWRITE, out protections[i]))
                        throw new InvalidOperationException("Cannot protect legacy tint instruction");
                    protectedCount++;
                }
                for(int i = 0; i < count; i++) if(!game.WriteBytes(image + sites[i], originals[i])) throw new InvalidOperationException("Cannot restore legacy tint instruction");
                if(!FlushInstructionCache(game.Handle, IntPtr.Zero, UIntPtr.Zero)) throw new InvalidOperationException("Cannot flush restored tint instructions");
                complete = true;
            } finally {
                try {
                    if(!complete) {
                        for(int i = 0; i < protectedCount; i++) game.WriteBytes(image + sites[i], before[i]);
                        FlushInstructionCache(game.Handle, IntPtr.Zero, UIntPtr.Zero);
                    }
                    for(int i = 0; i < protectedCount; i++) game.VirtualProtect(image + sites[i], originals[i].Length, protections[i]);
                } finally { WinAPI.NtResumeProcess(game.Handle); }
            }
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FlushInstructionCache(IntPtr process, IntPtr address, UIntPtr size);
    }
}
