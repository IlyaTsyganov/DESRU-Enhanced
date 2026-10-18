namespace DESpeedrunUtil.Overlay {
    // Windows x64 ABI adapter. Calls the original HUD renderer, then the game's text renderer.
    // No instructions in a game function are overwritten; only its aligned virtual slot is redirected.
    internal static class NativeSpeedometerCode {
        internal static readonly string[] Commands = {
            "desru_speedometer_show", "desru_speedometer_x_pos", "desru_speedometer_y_pos", "desru_speedometer_size", "desru_lowhptintopacity",
            "desru_speedometer_showlanding", "desru_speedometer_color"
        };
        internal const int StateSize = 4096, CallbackStart = 0x500, CallbackStride = 0x100;
        internal const string Magic = "DESRUS07";
        internal const int ConsoleReadyOffset = 416, ConsoleFormatOffset = 420, ConsoleTextOffset = 432, ConsoleTextCapacity = 576;

        internal sealed class Code {
            internal readonly List<byte> Bytes = new();
            private readonly Dictionary<string, int> _labels = new();
            private readonly List<(int Offset, string Label)> _fixups = new();
            internal void Emit(string hex) => Bytes.AddRange(Convert.FromHexString(hex.Replace(" ", "")));
            internal void Pointer(long value) => Bytes.AddRange(BitConverter.GetBytes(value));
            internal void Label(string name) => _labels.Add(name, Bytes.Count);
            internal void Jump(string instruction, string label) { Emit(instruction); _fixups.Add((Bytes.Count, label)); Emit("00000000"); }
            internal void Call(long function) { Emit("48B8"); Pointer(function); Emit("FFD0"); }
            internal byte[] Finish() {
                foreach(var fixup in _fixups) {
                    var displacement = BitConverter.GetBytes(_labels[fixup.Label] - fixup.Offset - 4);
                    for(int i = 0; i < 4; i++) Bytes[fixup.Offset + i] = displacement[i];
                }
                return Bytes.ToArray();
            }
        }

        internal static byte[] Render(long codeAddress, long state, long original, long draw, long width, long height, long commandSystem, long addCommand, long print = 0) {
            var c = new Code();
            // Entry RSP % 16 == 8. Three saved registers + 64 bytes give aligned call sites.
            c.Emit("535657 4883EC40 488BF2 48BB"); c.Pointer(state);
            c.Call(original);
            c.Emit("83BBC000000000"); c.Jump("0F8E", "tintHeartbeatDone");
            c.Emit("F0FF8BC0000000"); c.Label("tintHeartbeatDone");
            c.Emit("48FF4308 837B0403"); c.Jump("0F84", "registered");
            // 1 = four old commands, 2 = five, 3 = all seven. Existing command names
            // point into the shared string block, so editing slot zero also renames it.
            for(int i = 0; i < Commands.Length; i++) {
                if(i < 5) {
                    c.Emit(i < 4 ? "837B0401" : "837B0402"); c.Jump("0F8D", $"commandRegistered{i}");
                }
                c.Emit("48B9"); c.Pointer(commandSystem);
                c.Emit("48BA"); c.Pointer(state + 0x800 + i * 0x100);
                c.Emit("49B8"); c.Pointer(codeAddress + CallbackStart + i * CallbackStride);
                c.Emit("49B9"); c.Pointer(state + 0x800 + i * 0x100 + 64);
                // AddCommand(this, name, callback, description, autocomplete=null, CMD_NOCHEAT=2).
                c.Emit("48C744242000000000 C744242802000000"); c.Call(addCommand);
                if(i < 5) c.Label($"commandRegistered{i}");
            }
            c.Emit("C7430403000000");
            c.Label("registered");
            if(print != 0) {
                // Print on the game's own HUD thread even while the meters are hidden.
                // VPrintf(channel=1, severity=1, "%s", va_list{message}); never use user text as a format.
                c.Emit("83BBA001000000"); c.Jump("0F8E", "consoleDone");
                c.Emit("488D83B0010000 4889442430 B901000000 BA01000000 4C8D83A4010000 4C8D4C2430");
                c.Call(print);
                c.Emit("C783A001000000000000");
                c.Label("consoleDone");
            }
            c.Emit("833B00"); c.Jump("0F84", "done");
            c.Emit("F0FF0B 488BCE"); c.Call(width); c.Emit("F30F11442438");
            c.Emit("488BCE"); c.Call(height); c.Emit("F30F1144243C");
            for(int i = 0; i < 2; i++) {
                c.Emit("48BF"); c.Pointer(state + 64 + i * 64);
                c.Emit("833F00"); c.Jump("0F84", $"skip{i}");
                // Convert physical client pixels to the game's virtual HUD units, then clamp.
                // Header +56/+60 holds the physical viewport width/height (always positive).
                c.Emit("F30F10470C F30F594730 F30F104C2438 F30F5CC8 F30F5F4B18");
                c.Emit("F30F104704 F30F59442438 F30F5E4338 F30F5D C1 F30F5F4318 0F28C8");
                c.Emit("F30F10470C F30F594314 F30F1054243C F30F5CD0 F30F5F5318");
                c.Emit("F30F104708 F30F5944243C F30F5E433C F30F5DC2 F30F5F4318 0F28D0");
                c.Emit("488BCE 4C8D4F10 488D83"); c.Bytes.AddRange(BitConverter.GetBytes(384 + i * 16));
                c.Emit("4889442420 C744242801000000 F30F10470C F30F11442430");
                c.Call(draw);
                c.Label($"skip{i}");
            }
            c.Label("done"); c.Emit("4883C440 5F5E5B C3");
            byte[] result = c.Finish();
            if(result.Length >= CallbackStart) throw new InvalidOperationException("Native renderer exceeds its code region");
            return result;
        }

        internal static byte[] Command(long mailbox, int argumentCount = 2) {
            var c = new Code();
            c.Emit("535657 488BD9"); // Preserve nonvolatile registers; RBX = idCmdArgs*.
            c.Emit("48BF"); c.Pointer(mailbox);
            c.Emit("8B03 894704"); // argc, including the command name; publish sequence after copying.
            for(int i = 0; i < argumentCount; i++) {
                c.Emit("48BF"); c.Pointer(mailbox + 16 + i * 32);
                c.Emit("C60700 C6471F00 833B"); c.Emit((i + 2).ToString("X2"));
                c.Jump("0F8C", $"next{i}");
                c.Emit("488B73"); c.Emit((16 + i * 8).ToString("X2")); // argv[1], argv[2].
                c.Emit("4885F6"); c.Jump("0F84", $"next{i}");
                c.Emit("B91F000000");
                c.Label($"copy{i}"); c.Emit("ACAA 84C0"); c.Jump("0F84", $"next{i}");
                c.Emit("FFC9"); c.Jump("0F85", $"copy{i}"); c.Emit("C60700");
                c.Label($"next{i}");
            }
            c.Emit("48B8"); c.Pointer(mailbox); c.Emit("F0FF00 5F5E5B C3");
            byte[] result = c.Finish();
            if(result.Length > (argumentCount == 4 ? 0x500 : CallbackStride)) throw new InvalidOperationException("Native command exceeds its code region");
            return result;
        }
    }
}
