using System.Diagnostics;

namespace DESpeedrunUtil.Memory {
    internal static class PerformanceMetricsDriverName {
        // GPUVendor offsets identify idStr's 20-byte inline buffer, not its data pointer.
        // Longer names (including "AMD proprietary driver") live in a heap allocation.
        internal static bool Hide(Process game, IntPtr inlineBuffer) {
            if(inlineBuffer == IntPtr.Zero || !game.ReadBytes(inlineBuffer - 16, 16, out var header)) return false;
            var data = new IntPtr(BitConverter.ToInt64(header, 0));
            int length = BitConverter.ToInt32(header, 8);
            int capacity = BitConverter.ToInt32(header, 12) & 0x3fffffff; // idStr allocation flags
            if(data.ToInt64() <= 0 || length < 0 || length > 4096 || capacity <= length || capacity > 4096) return false;
            if(data == inlineBuffer ? capacity != 20 : capacity < 20) return false;
            if(!game.ReadValue<byte>(data, out var first)) return false;
            // Rendering consumes a C string. Keep idStr's pointer, length, allocation and
            // neighboring strings intact; resolve the pointer again on every update.
            return first == 0 || game.WriteBytes(data, new byte[] { 0 });
        }
    }
}
