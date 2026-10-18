using DESpeedrunUtil.Memory;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Serilog;

namespace DESpeedrunUtil.Overlay {
    internal sealed class NativeSpeedometer : IDisposable {
        private readonly Process _game;
        private readonly IntPtr _state;
        private readonly int[] _sequences = new int[NativeSpeedometerCode.Commands.Length];
        private readonly Queue<string> _consoleMessages = new();
        private bool _tintAttached, _tintAttachFailed;
        private readonly LowHealthTintTarget _tintTarget;
        private readonly NativeGameLayout _layout;
        internal int GroundPlaneFromVelocity => _layout.GroundPlaneFromVelocity;
        internal bool TintAttached => _tintAttached;
        private readonly float _glyphWidth, _glyphHeight;
        internal const int ModuleSize11 = 492113920;
        internal bool CommandsRegistered => _game.ReadValue<int>(_state + 4) == 3;
        internal long RenderCount => _game.ReadValue<long>(_state + 8);

        private NativeSpeedometer(Process game, IntPtr state, NativeGameLayout layout) {
            _game = game; _state = state;
            _layout = layout;
            _tintTarget = new LowHealthTintTarget(game);
            _glyphWidth = game.ReadValue<float>(game.MainModule!.BaseAddress + layout.GlyphWidth);
            _glyphHeight = game.ReadValue<float>(game.MainModule.BaseAddress + layout.GlyphHeight);
            if(!float.IsFinite(_glyphWidth) || _glyphWidth <= 0 || _glyphWidth > 1000 ||
                !float.IsFinite(_glyphHeight) || _glyphHeight <= 0 || _glyphHeight > 1000)
                throw new InvalidOperationException("Cannot read game font metrics");
            // Saved user settings are authoritative on reconnect; do not replay old console requests.
            for(int i = 0; i < _sequences.Length; i++) _sequences[i] = game.ReadValue<int>(state + 0x400 + i * 0x80);
        }

        internal static NativeSpeedometer Attach(Process game) {
            var layout = NativeGameLayout.Resolve(game.MainModule!.FileName);
            if(game.MainModule.ModuleMemorySize != layout.ImageSize) throw new NotSupportedException("On-disk and loaded game images differ");
            IntPtr image = game.MainModule.BaseAddress, slot = image + layout.HudSlot;
            if(!game.ReadPointer(slot, true, out var original)) throw new InvalidOperationException("Cannot read HUD renderer");
            IntPtr previousState = IntPtr.Zero;
            if(original != image + layout.Render) {
                // Reconnect after DESRU restarted. Code/data remain alive for the lifetime of the game.
                var existing = game.ReadBytes(original, 20);
                if(existing != null && existing.AsSpan(0, 12).SequenceEqual(Convert.FromHexString("5356574883EC40488BF248BB"))) {
                    IntPtr state = new(BitConverter.ToInt64(existing, 12));
                    string magic = Encoding.ASCII.GetString(game.ReadBytes(state + 48, 8) ?? Array.Empty<byte>());
                    if(magic == NativeSpeedometerCode.Magic) {
                        byte[] descriptions = CreateState();
                        for(int i = 0; i < NativeSpeedometerCode.Commands.Length; i++) game.WriteBytes(state + 0x800 + i * 0x100, descriptions.AsSpan(0x800 + i * 0x100, 256).ToArray());
                        return new NativeSpeedometer(game, state, layout);
                    }
                    // Upgrade earlier coordinate adapters without freeing their registered
                    // command callbacks. Reuse their mailboxes; replace only the aligned render slot.
                    if(magic == "DESRUS02" || magic == "DESRUS03" || magic == "DESRUS04" || magic == "DESRUS05" || magic == "DESRUS06") previousState = state;
                }
                if(previousState == IntPtr.Zero) throw new NotSupportedException("Another HUD renderer hook is installed; it was left unchanged");
            }
            foreach(int code in new[] { layout.Render, layout.Draw, layout.Width, layout.Height, layout.Print }) VerifyFileCode(game, code);
            if(!game.ReadPointer(image + layout.CommandGlobal, true, out var commandSystem) || commandSystem == IntPtr.Zero ||
                !game.ReadPointer(commandSystem, true, out var commandVtable) ||
                !game.ReadPointer(commandVtable + 24, true, out var addCommand) || addCommand == IntPtr.Zero)
                throw new InvalidOperationException("Cannot resolve the game command system");
            IntPtr allocation = game.AllocateMemory(8192);
            if(allocation == IntPtr.Zero) throw new InvalidOperationException("Cannot allocate the HUD adapter");
            bool installed = false;
            try {
                IntPtr state = previousState != IntPtr.Zero ? previousState : allocation + 4096;
                byte[] data = CreateState();
                if(previousState == IntPtr.Zero && !game.WriteBytes(state, data)) throw new InvalidOperationException("Cannot initialize HUD state");
                byte[] code = NativeSpeedometerCode.Render(allocation.ToInt64(), state.ToInt64(), (image + layout.Render).ToInt64(),
                    (image + layout.Draw).ToInt64(), (image + layout.Width).ToInt64(), (image + layout.Height).ToInt64(), commandSystem.ToInt64(), addCommand.ToInt64(), (image + layout.Print).ToInt64());
                if(!game.WriteBytes(allocation, code)) throw new InvalidOperationException("Cannot write the HUD adapter");
                for(int i = 0; i < NativeSpeedometerCode.Commands.Length; i++)
                    if(!game.WriteBytes(allocation + NativeSpeedometerCode.CallbackStart + i * NativeSpeedometerCode.CallbackStride,
                        NativeSpeedometerCode.Command((state + 0x400 + i * 0x80).ToInt64(), i == 6 ? 4 : 2))) throw new InvalidOperationException("Cannot initialize console commands");
                if(!game.VirtualProtect(allocation, 4096, MemPageProtect.PAGE_EXECUTE_READ) ||
                    !game.VirtualProtect(state, 4096, MemPageProtect.PAGE_READWRITE) || !FlushInstructionCache(game.Handle, allocation, (UIntPtr) 4096))
                    throw new InvalidOperationException("Cannot protect HUD adapter memory");
                if(!game.ReadPointer(slot, true, out var check) || check != original) throw new InvalidOperationException("HUD renderer changed while attaching");
                if(previousState != IntPtr.Zero) {
                    // Quiesce drawing and initialize the new viewport fields before publishing code.
                    game.WriteBytes(state, new byte[4]);
                    game.WriteBytes(state + 56, data.AsSpan(56, 8).ToArray());
                    game.WriteBytes(state + 384, data.AsSpan(384, 32).ToArray());
                    if(!game.WriteBytes(state + NativeSpeedometerCode.ConsoleReadyOffset, data.AsSpan(NativeSpeedometerCode.ConsoleReadyOffset, 592).ToArray()))
                        throw new InvalidOperationException("Cannot initialize console response buffer");
                    // New registration may run on the next game frame immediately after the slot
                    // write. Publish the fifth name/help before exposing the new renderer.
                    for(int i = 0; i < NativeSpeedometerCode.Commands.Length; i++)
                        if(!game.WriteBytes(state + 0x800 + i * 0x100, data.AsSpan(0x800 + i * 0x100, 256).ToArray()))
                            throw new InvalidOperationException("Cannot publish console command names");
                }
                if(!game.VirtualProtect(slot, 8, MemPageProtect.PAGE_READWRITE, out var previous)) throw new InvalidOperationException("Cannot update HUD slot");
                try { installed = game.WriteBytes(slot, BitConverter.GetBytes(allocation.ToInt64())); }
                finally { game.VirtualProtect(slot, 8, previous); }
                if(!installed) throw new InvalidOperationException("Cannot attach the HUD adapter");
                if(previousState != IntPtr.Zero) {
                    game.WriteBytes(state + 48, Encoding.ASCII.GetBytes(NativeSpeedometerCode.Magic));
                }
                return new NativeSpeedometer(game, state, layout);
            } finally {
                if(!installed) game.FreeMemory(allocation);
            }
        }

        internal static byte[] CreateState() {
            var data = new byte[4096];
            BitConverter.GetBytes(384f).CopyTo(data, 16);
            BitConverter.GetBytes(48f).CopyTo(data, 20);
            BitConverter.GetBytes(1920f).CopyTo(data, 56);
            BitConverter.GetBytes(1080f).CopyTo(data, 60);
            BitConverter.GetBytes(1f).CopyTo(data, LowHealthTintCode.ScaleOffset);
            for(int i = 0; i < 4; i++) BitConverter.GetBytes(1f).CopyTo(data, 32 + i * 4);
            for(int i = 0; i < 8; i++) BitConverter.GetBytes(1f).CopyTo(data, 384 + i * 4);
            Encoding.ASCII.GetBytes(NativeSpeedometerCode.Magic).CopyTo(data, 48);
            Encoding.ASCII.GetBytes("%s\0").CopyTo(data, NativeSpeedometerCode.ConsoleFormatOffset);
            for(int i = 0; i < NativeSpeedometerCode.Commands.Length; i++) {
                Encoding.ASCII.GetBytes(NativeSpeedometerCode.Commands[i]).CopyTo(data, 0x800 + i * 0x100);
                string help = i == 4 ? "<0..100>: low-health red tint opacity; 100 restores original opacity" :
                    i == 5 ? "<0|1>: include last-landing speed (preserved while hidden)" :
                    i == 6 ? "<R G B> [current|landing]: text color, integer channels 0..255" :
                    i == 0 ? "<0|1> [current|landing]: show/hide DESRU numbers" :
                    i == 3 ? "<0.1..10>: text scale of both speedometers" : "<pixels> [current|landing]: position from the top-left corner (both if omitted)";
                Encoding.ASCII.GetBytes(help).CopyTo(data, 0x840 + i * 0x100);
            }
            return data;
        }

        private static void VerifyFileCode(Process game, int rva) {
            using var file = File.OpenRead(game.MainModule!.FileName);
            using var pe = new System.Reflection.PortableExecutable.PEReader(file, System.Reflection.PortableExecutable.PEStreamOptions.LeaveOpen);
            var section = pe.PEHeaders.SectionHeaders.Single(s => rva >= s.VirtualAddress && rva + 16 <= s.VirtualAddress + s.SizeOfRawData);
            file.Position = rva - section.VirtualAddress + section.PointerToRawData;
            var expected = new byte[16];
            if(file.Read(expected, 0, expected.Length) != expected.Length ||
                !(game.ReadBytes(game.MainModule.BaseAddress + rva, expected.Length) ?? Array.Empty<byte>()).SequenceEqual(expected))
                throw new NotSupportedException("Game code differs from the resolved executable; another hook was left unchanged");
        }

        internal void Update(SpeedometerReading current, SpeedometerReading landing, double lowHpTintOpacity = 100, IntPtr player = default) {
            FlushConsoleMessage();
            lowHpTintOpacity = double.IsFinite(lowHpTintOpacity) ? Math.Clamp(lowHpTintOpacity, 0, 100) : 100;
            if(lowHpTintOpacity < 100 && !_tintAttached && !_tintAttachFailed) {
                try {
                    LowHealthTintHook.Attach(_game, _state);
                    LowHealthHudHook.Attach(_game, _state);
                    _tintAttached = true;
                }
                catch(Exception e) {
                    _tintAttachFailed = true;
                    Log.Warning(e, "Low-health tint control is unavailable; original tint and speedometers remain active");
                }
            }
            byte[] tint = new byte[8];
            if(_tintAttached) _tintTarget.Update(_state, player);
            BitConverter.GetBytes(_tintAttachFailed ? 0 : 240).CopyTo(tint, 0);
            BitConverter.GetBytes((float) (_tintAttachFailed ? 1 : lowHpTintOpacity / 100)).CopyTo(tint, 4);
            byte[] readings = CreateReadings(current, landing, _glyphWidth);
            if(!SpeedometerOverlay.TryGetPhysicalViewport(_game.MainWindowHandle, out var viewport)) viewport = new Rectangle(0, 0, 1920, 1080);
            byte[] dimensions = new byte[8];
            BitConverter.GetBytes((float) viewport.Width).CopyTo(dimensions, 0);
            BitConverter.GetBytes((float) viewport.Height).CopyTo(dimensions, 4);
            if(!_game.WriteBytes(_state + LowHealthTintCode.LeaseOffset, tint) ||
                !_game.WriteBytes(_state + 20, BitConverter.GetBytes(_glyphHeight)) || !_game.WriteBytes(_state + 64, readings) ||
                !_game.WriteBytes(_state + 384, CreateColors(current, landing)) ||
                !_game.WriteBytes(_state + 56, dimensions) ||
                !_game.WriteBytes(_state, BitConverter.GetBytes(current.Enabled || landing.Enabled ? 240 : 0)))
                throw new InvalidOperationException("Cannot update HUD values");
        }

        internal static byte[] CreateReadings(SpeedometerReading current, SpeedometerReading landing, float glyphWidth) {
            byte[] data = new byte[128];
            WriteReading(data, 0, current, glyphWidth);
            WriteReading(data, 64, landing, glyphWidth);
            return data;
        }

        internal static byte[] CreateColors(SpeedometerReading current, SpeedometerReading landing) {
            byte[] data = new byte[32];
            var colors = new[] { current.DisplayColor, landing.DisplayColor };
            for(int i = 0; i < 2; i++) {
                BitConverter.GetBytes(colors[i].R / 255f).CopyTo(data, i * 16);
                BitConverter.GetBytes(colors[i].G / 255f).CopyTo(data, i * 16 + 4);
                BitConverter.GetBytes(colors[i].B / 255f).CopyTo(data, i * 16 + 8);
                BitConverter.GetBytes(1f).CopyTo(data, i * 16 + 12);
            }
            return data;
        }

        private static void WriteReading(byte[] data, int offset, SpeedometerReading reading, float glyphWidth) {
            string text = SpeedometerText.Format(reading);
            BitConverter.GetBytes(reading.Enabled ? 1 : 0).CopyTo(data, offset);
            BitConverter.GetBytes((float) Math.Max(0, reading.X)).CopyTo(data, offset + 4);
            BitConverter.GetBytes((float) Math.Max(0, reading.Y)).CopyTo(data, offset + 8);
            BitConverter.GetBytes((float) Math.Clamp(reading.Scale, 0.1, 10) * 0.5f).CopyTo(data, offset + 12);
            Encoding.ASCII.GetBytes(text[..Math.Min(text.Length, 31)]).CopyTo(data, offset + 16);
            BitConverter.GetBytes(text.Length * glyphWidth).CopyTo(data, offset + 48);
        }

        internal IEnumerable<(int Command, string[] Arguments)> ReadCommands() {
            for(int i = 0; i < _sequences.Length; i++) {
                var data = _game.ReadBytes(_state + 0x400 + i * 0x80, i == 6 ? 144 : 80);
                if(data == null) continue;
                int sequence = BitConverter.ToInt32(data, 0), argc = BitConverter.ToInt32(data, 4);
                if(sequence == _sequences[i] ||
                    !_game.ReadValue<int>(_state + 0x400 + i * 0x80, out var confirmed) || confirmed != sequence) continue;
                _sequences[i] = sequence;
                if(argc < 1 || argc > (i == 6 ? 5 : 3)) continue;
                yield return (i, Enumerable.Range(0, argc - 1).Select(n => ReadArgument(data, 16 + n * 32)).ToArray());
            }
        }

        private static string ReadArgument(byte[] data, int offset) {
            int end = Array.IndexOf(data, (byte) 0, offset, 32);
            return Encoding.ASCII.GetString(data, offset, end >= offset ? end - offset : 31);
        }

        internal void QueueConsoleMessage(string message) {
            if(Encoding.ASCII.GetByteCount(message) >= NativeSpeedometerCode.ConsoleTextCapacity)
                throw new ArgumentException("Console response exceeds its shared buffer", nameof(message));
            // A bounded host queue preserves several different help requests until HUD acknowledges them.
            if(_consoleMessages.Count < 32) _consoleMessages.Enqueue(message);
        }

        private void FlushConsoleMessage() {
            if(_consoleMessages.Count == 0 || !_game.ReadValue<int>(_state + NativeSpeedometerCode.ConsoleReadyOffset, out var ready) || ready != 0) return;
            byte[] text = Encoding.ASCII.GetBytes(_consoleMessages.Peek() + "\0");
            if(!_game.WriteBytes(_state + NativeSpeedometerCode.ConsoleTextOffset, text) ||
                !_game.WriteBytes(_state + NativeSpeedometerCode.ConsoleReadyOffset, BitConverter.GetBytes(1)))
                throw new InvalidOperationException("Cannot publish console response");
            _consoleMessages.Dequeue();
        }

        public void Dispose() {
            try {
                if(!_game.HasExited) {
                    _game.WriteBytes(_state + LowHealthTintCode.LeaseOffset, new byte[4]);
                    _game.WriteBytes(_state, new byte[4]);
                }
            }
            catch(InvalidOperationException) { }
            catch(System.ComponentModel.Win32Exception) { }
            // Do not free code that a game thread/registered command could still be executing.
            // A reconnect reuses this 8 KiB block; the OS releases it when the game exits.
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FlushInstructionCache(IntPtr process, IntPtr address, UIntPtr size);
    }
}
