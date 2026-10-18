namespace DESpeedrunUtil.Overlay {
    internal static class LowHealthTintCode {
        internal const int LeaseOffset = 192, ScaleOffset = 196;

        internal static byte[] CreateHudControl(long state, long vtable, long continuation, int swfRootOffset = 0x4D0) {
            var c = new NativeSpeedometerCode.Code();
            // No native calls: preserve flags, all touched GP registers, and the whole XMM0.
            c.Emit("9C5051524150415141524153 4883EC10 0F110424 48B8"); c.Pointer(state);
            c.Emit("48FF8028010000 48BA"); c.Pointer(vtable);
            c.Emit("483913"); c.Jump("0F85", "done");
            c.Emit("48899818010000 488B5310 48899020010000 48FF8030010000 4885D2"); c.Jump("0F84", "done");
            c.Emit("4C8B8A"); c.Bytes.AddRange(BitConverter.GetBytes(swfRootOffset));
            // Resolve live display entries on the game's own UI thread. No stale host-side
            // sprite pointer is ever followed after a HUD reload or a timeline change.
            FindChild("lowHealth", "health");
            FindChild("backgroundFx", "background");
            c.Emit("4C398038010000"); c.Jump("0F85", "capture");
            c.Emit("48398850010000"); c.Jump("0F85", "capture");
            c.Emit("448B5118 443B9048010000"); c.Jump("0F85", "capture");
            c.Emit("448B5128 443B904C010000"); c.Jump("0F84", "apply");
            c.Label("capture");
            c.Emit("4C898038010000 48898850010000 448B5118 44899040010000 448B5128 44899044010000");
            c.Label("apply");
            c.Emit("48FF8068010000");
            for(int i = 0; i < 2; i++) {
                c.Emit(i == 0 ? "F30F108040010000" : "F30F108044010000");
                c.Emit("83B8C000000000"); c.Jump("0F8E", $"unscaled{i}");
                c.Emit("F30F5980C4000000");
                c.Label($"unscaled{i}");
                c.Emit(i == 0 ? "F30F114118 F30F118048010000" : "F30F114128 F30F11804C010000");
            }
            c.Label("done");
            c.Emit("0F100424 4883C410 415B415A415941585A59589D 488B43104885C0 FF2500000000"); c.Pointer(continuation);
            byte[] result = c.Finish();
            if(result.Length > 0x300) throw new InvalidOperationException("HUD tint bridge exceeds its code region");
            return result;

            void FindChild(string name, string label) {
                c.Emit("4D85C9"); c.Jump("0F84", "done");
                c.Emit("498B4970 418B5178 85D2"); c.Jump("0F8E", "done");
                c.Emit("81FA00010000"); c.Jump("0F87", "done");
                c.Emit("4885C9"); c.Jump("0F84", "done");
                c.Label(label + "Loop");
                c.Emit("4C8B4130 4D85C0"); c.Jump("0F84", label + "Next");
                c.Emit("4D8B5818 4D85DB"); c.Jump("0F84", label + "Next");
                c.Emit("49BA"); c.Pointer(BitConverter.ToInt64(System.Text.Encoding.ASCII.GetBytes(name[..8])));
                c.Emit("4D3913"); c.Jump("0F85", label + "Next");
                if(name == "lowHealth") c.Emit("6641837B0868");
                else c.Emit("41817B086E644678");
                c.Jump("0F85", label + "Next");
                if(name != "lowHealth") { c.Emit("41807B0C00"); c.Jump("0F85", label + "Next"); }
                c.Emit("4D8BC8"); c.Jump("E9", label + "Found");
                c.Label(label + "Next");
                c.Emit("4883C140 FFCA"); c.Jump("0F85", label + "Loop"); c.Jump("E9", "done");
                c.Label(label + "Found");
            }
        }

        internal static byte[] CreateHudDiscovery(long state, long vtable, long continuation) {
            var b = new List<byte>();
            void E(string hex) => b.AddRange(Convert.FromHexString(hex.Replace(" ", "")));
            void P(long value) => b.AddRange(BitConverter.GetBytes(value));
            E("9C5052 48B8"); P(state); E("48FF8028010000 48BA"); P(vtable);
            E("483913 7519 48899818010000 488B5310 48899020010000 48FF8030010000");
            E("5A589D 488B43104885C0 FF2500000000"); P(continuation);
            return b.ToArray();
        }

        internal static byte[] CreateVignette(long state, long continuation, bool preserveBookkeeping = true, bool scaleColor = false, int bufferOffset = 0x2DF8) {
            var bytes = new List<byte>();
            var exits = new List<int>();
            void Emit(string hex) => bytes.AddRange(Convert.FromHexString(hex.Replace(" ", "")));
            void Fix(int offset, int target) { byte[] delta = BitConverter.GetBytes(target - offset - 4); for(int i = 0; i < 4; i++) bytes[offset + i] = delta[i]; }
            void Exit(string condition) { Emit(condition); exits.Add(bytes.Count); Emit("00000000"); }
            Emit("9C5052 48B8"); bytes.AddRange(BitConverter.GetBytes(state));
            Emit("4889B8C8000000 48FF80E0000000 83B8C000000000");
            Exit("0F8E"); // Independent controller heartbeat.
            Emit("486353C4 83FA07"); Exit("0F87"); // Selected vignette index, including -1.
            Emit("486BD250 4803D7"); // RDX = idView + index * sizeof(screenVignette_t).
            Emit("488B92"); bytes.AddRange(BitConverter.GetBytes(bufferOffset)); Emit("483B90E8000000"); Exit("0F85"); // Exact screen-FX group handle.
            Emit("486353C4 486BD250 4803D7 8B92"); bytes.AddRange(BitConverter.GetBytes(bufferOffset + 8)); // declActionIdx.
            Emit("3B90F0000000 0F84"); int match = bytes.Count; Emit("00000000");
            Emit("3B90F4000000"); Exit("0F85");
            Fix(match, bytes.Count);
            Emit("48FF80F8000000 F3410F104610 F30F118000010000");
            if(preserveBookkeeping) Emit("4883EC10 F30F110424");
            if(scaleColor) {
                foreach(byte offset in new byte[] { 4, 8, 12 }) {
                    Emit("F3410F1046"); bytes.Add(offset);
                    Emit("F30F5980C4000000 F3410F1146"); bytes.Add(offset);
                }
                Emit("F30F100424");
            }
            Emit("F30F5980C4000000 F3410F114610 F30F118004010000");
            if(preserveBookkeeping) {
                // The following game branch counts nonempty layer records. Keep its original
                // alpha in XMM0 even for scale zero so the renderer receives a transparent record.
                Emit("F30F100424 4883C410 5A589D FF2500000000"); bytes.AddRange(BitConverter.GetBytes(continuation));
            }
            int done = bytes.Count;
            foreach(int offset in exits) Fix(offset, done);
            Emit("5A589D F3410F104610 FF2500000000"); bytes.AddRange(BitConverter.GetBytes(continuation));
            return bytes.ToArray();
        }

        internal static byte[] CreateIndexedVignette(long state, long address, long continuation, int bufferOffset, int alphaOffset) {
            // Newer builds use RSI=view, RDI=layer+3C, and an indexed R14/R15 output.
            // Adapt those registers to the already tested bridge, then restore them before game continuation.
            var prefix = new List<byte>(Convert.FromHexString("53574156488BDF488BFE4F8DB4BE"));
            prefix.AddRange(BitConverter.GetBytes(checked(alphaOffset - 0x10)));
            int length = CreateVignette(state, 0, bufferOffset: bufferOffset).Length;
            byte[] body = CreateVignette(state, address + prefix.Count + length, bufferOffset: bufferOffset);
            prefix.AddRange(body);
            prefix.AddRange(Convert.FromHexString("415E5F5BFF2500000000"));
            prefix.AddRange(BitConverter.GetBytes(continuation));
            if(prefix.Count > 0x300) throw new InvalidOperationException("Indexed vignette bridge exceeds its code region");
            return prefix.ToArray();
        }

        // Legacy byte encoders below are retained solely to recognize and remove the owned
        // DESRUT01/02/03 experimental hooks during upgrade. They are never newly installed.
        // A JMP bridge for exactly one verified alpha instruction. Preserve flags and RAX;
        // XMM0 is the original instruction's result, all other registers remain untouched.
        internal static byte[] Create(long state, long continuation, long normalizationConstant, bool combined, bool telemetry = true) {
            var bytes = new List<byte>();
            void Emit(string hex) => bytes.AddRange(Convert.FromHexString(hex));
            void Pointer(long value) => bytes.AddRange(BitConverter.GetBytes(value));
            Emit("9C50"); // pushfq; push rax
            if(telemetry) {
                Emit("48B8"); Pointer(state);
                Emit("4889B8C8000000"); // Record the actual idView for read-only diagnostics.
                Emit(combined ? "48FF80D0000000" : "48FF80D8000000");
            }
            if(combined) {
                Emit("48B8"); Pointer(normalizationConstant);
                Emit("F30F5900"); // original: mulss xmm0, [normalizationConstant] (1/255)
            } else Emit("F3410F5EC2"); // original: divss xmm0, xmm10 (255)
            Emit("48B8"); Pointer(state);
            Emit("83B8C000000000"); // tint lease > 0?
            Emit("7E08");
            Emit("F30F5980C4000000"); // mulss xmm0, [state + ScaleOffset]
            Emit("589DFF2500000000"); Pointer(continuation); // restore; absolute indirect JMP
            return bytes.ToArray();
        }

        internal static byte[] CreatePostProcess(long state, long continuation) {
            var bytes = new List<byte>();
            void Emit(string hex) => bytes.AddRange(Convert.FromHexString(hex));
            void Pointer(long value) => bytes.AddRange(BitConverter.GetBytes(value));
            Emit("9C50 0F108688350000 48B8".Replace(" ", "")); Pointer(state);
            Emit("4889B0C8000000 48FF80E0000000 83B8C000000000".Replace(" ", ""));
            // Skip the entire scalar edit if the controller's heartbeat expired.
            byte[] edit = Convert.FromHexString("4883EC100F110424F30F10442404F30F5980C4000000F30F114424040F1004244883C410");
            bytes.Add(0x7E); bytes.Add(checked((byte) edit.Length)); bytes.AddRange(edit);
            Emit("589DFF2500000000"); Pointer(continuation);
            return bytes.ToArray();
        }
    }
}
