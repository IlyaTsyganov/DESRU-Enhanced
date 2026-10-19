using System.Buffers.Binary;

namespace DESpeedrunUtil.Overlay {
    internal sealed record NativePlayerCheatFlag(int Offset, byte Mask) {
        internal static NativePlayerCheatFlag Decode(ReadOnlySpan<byte> code) {
            // Reflected bool getters: movzx eax,byte [rcx+offset]; shr eax,n; and eax,1; ret.
            if(code.Length < 14 || !code[..3].SequenceEqual(new byte[] { 0x0f, 0xb6, 0x81 }))
                throw new NotSupportedException("Unsupported player cheat getter");
            int offset = BinaryPrimitives.ReadInt32LittleEndian(code[3..7]);
            int shift, at;
            if(code[7] == 0xd1 && code[8] == 0xe8) { shift = 1; at = 9; }
            else if(code[7] == 0xc1 && code[8] == 0xe8) { shift = code[9]; at = 10; }
            else throw new NotSupportedException("Unsupported player cheat bit shift");
            if(offset <= 0 || offset >= 1024 * 1024 || shift > 7 || !code.Slice(at, 4).SequenceEqual(new byte[] { 0x83, 0xe0, 1, 0xc3 }))
                throw new NotSupportedException("Unsupported player cheat bit mask");
            return new(offset, (byte) (1 << shift));
        }
    }
    internal sealed record NativePlayerCheatLayout(NativePlayerCheatFlag Noclip, NativePlayerCheatFlag Notarget, NativePlayerCheatFlag God);
}
