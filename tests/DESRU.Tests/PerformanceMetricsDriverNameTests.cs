using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using DESpeedrunUtil.Memory;

internal static class PerformanceMetricsDriverNameTests {
    internal static void Run(Action<bool, string> check) {
        using var process = Process.GetCurrentProcess();
        IntPtr owner = Marshal.AllocHGlobal(96), heap = Marshal.AllocHGlobal(32), replacement = Marshal.AllocHGlobal(32);
        try {
            void Initialize(IntPtr data, string text, int capacity) {
                Marshal.Copy(Enumerable.Repeat((byte) 0x5a, 96).ToArray(), 0, owner, 96);
                Marshal.WriteInt64(owner, data.ToInt64());
                Marshal.WriteInt32(owner + 8, text.Length);
                Marshal.WriteInt32(owner + 12, unchecked((int) (data == owner + 16 ? 0x80000000 : 0xc0000000)) | capacity);
                byte[] bytes = Encoding.ASCII.GetBytes(text + '\0');
                Marshal.Copy(bytes, 0, data, bytes.Length);
            }
            byte[] Read(IntPtr address, int length) {
                byte[] result = new byte[length]; Marshal.Copy(address, result, 0, length); return result;
            }
            IntPtr inline = owner + 16;
            Initialize(inline, "NVIDIA", 20);
            byte[] before = Read(owner, 96);
            check(PerformanceMetricsDriverName.Hide(process, inline) && Marshal.ReadByte(inline) == 0,
                "Inline driver names are hidden");
            before[16] = 0;
            check(Read(owner, 96).SequenceEqual(before), "Hiding inline names preserves headers and neighboring idStr objects");

            Initialize(heap, "AMD proprietary driver", 32);
            before = Read(owner, 96);
            byte[] heapBefore = Read(heap, 32);
            check(PerformanceMetricsDriverName.Hide(process, inline) && Marshal.ReadByte(heap) == 0,
                "Heap-backed AMD driver names are hidden");
            heapBefore[0] = 0;
            check(Read(owner, 96).SequenceEqual(before) && Read(heap, 32).SequenceEqual(heapBefore),
                "Hiding AMD names changes only the first character of their actual buffer");
            check(PerformanceMetricsDriverName.Hide(process, inline), "Repeated updates accept an already hidden driver name");

            byte[] replacementText = Encoding.ASCII.GetBytes("AMD proprietary driver\0");
            Marshal.Copy(replacementText, 0, replacement, replacementText.Length);
            Marshal.WriteInt64(owner, replacement.ToInt64());
            check(PerformanceMetricsDriverName.Hide(process, inline) && Marshal.ReadByte(replacement) == 0,
                "Driver name allocation changes are resolved on the next update");

            Initialize(inline, "NVIDIA", 20);
            Marshal.WriteInt32(owner + 8, 20);
            before = Read(owner, 96);
            check(!PerformanceMetricsDriverName.Hide(process, inline) && Read(owner, 96).SequenceEqual(before),
                "Invalid string lengths are rejected without writing");
            Marshal.WriteInt32(owner + 8, 6); Marshal.WriteInt32(owner + 12, 64);
            check(!PerformanceMetricsDriverName.Hide(process, inline), "Unexpected inline layouts are rejected");
            Marshal.WriteInt64(owner, 0);
            check(!PerformanceMetricsDriverName.Hide(process, inline) && !PerformanceMetricsDriverName.Hide(process, IntPtr.Zero),
                "Null driver name pointers are ignored");
        } finally {
            Marshal.FreeHGlobal(replacement); Marshal.FreeHGlobal(heap); Marshal.FreeHGlobal(owner);
        }
    }
}
