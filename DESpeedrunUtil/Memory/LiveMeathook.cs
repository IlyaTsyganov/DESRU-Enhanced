using DESpeedrunUtil.Overlay;
using System.Diagnostics;
using System.Reflection.PortableExecutable;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace DESpeedrunUtil.Memory {
    // Late entry for the exact bundled upstream DLL. Keep the CRT entry and upstream
    // scanners; bypass only the startup-only GetSystemInfo/gamelib initialization path.
    internal sealed class LiveMeathook {
        internal const string OriginalHash = "02c7159964245e249bcffc12598651b871f6c73925c4e04355510587114cdab3";
        private const int PatchHead = 0x661b8, FramePointerGlobal = 0x68bb0;
        private readonly Process _game;
        private readonly NativeHudExtension _hud;
        private readonly NativeGameLayout _layout;
        private IntPtr _module, _frameTarget, _noOp;
        private byte[]? _frameOriginal;
        private IntPtr _savedState;
        private readonly List<Patch> _patches = new();
        private readonly List<Patch> _commands = new();
        internal bool Active { get; private set; }
        private sealed record Patch(IntPtr Address, byte[] Before, byte[] After);

        internal LiveMeathook(Process game, NativeHudExtension hud, NativeGameLayout layout) {
            _game = game; _hud = hud; _layout = layout;
            _savedState = game.ReadPointer(hud.State + 128);
            if(_savedState != IntPtr.Zero) {
                byte[] header = game.ReadBytes(_savedState, 24) ?? throw new InvalidOperationException("Cannot reconnect Meathook state");
                if(Encoding.ASCII.GetString(header, 0, 8) != "DESRUMH1") throw new NotSupportedException("Unknown Meathook session state");
                int size = BitConverter.ToInt32(header, 20);
                if(size < 32 || size > 1024 * 1024) throw new InvalidOperationException("Invalid Meathook session size");
                byte[] data = game.ReadBytes(_savedState, size) ?? throw new InvalidOperationException("Cannot read Meathook session");
                using var reader = new BinaryReader(new MemoryStream(data)); reader.ReadBytes(8);
                _module = new IntPtr(reader.ReadInt64()); Active = reader.ReadInt32() == 1; reader.ReadInt32();
                void ReadPatches(List<Patch> output) {
                    int count = reader.ReadInt32(); if(count < 0 || count > 65536) throw new InvalidOperationException("Invalid saved patch count");
                    for(int i = 0; i < count; i++) {
                        IntPtr address = new(reader.ReadInt64()); int length = reader.ReadInt32();
                        if(length < 1 || length > 65536 || reader.BaseStream.Position + 2L * length > size) throw new InvalidOperationException("Invalid saved patch");
                        output.Add(new(address, reader.ReadBytes(length), reader.ReadBytes(length)));
                    }
                }
                ReadPatches(_patches); ReadPatches(_commands);
                if(!_game.Modules.Cast<ProcessModule>().Any(m => m.BaseAddress == _module && IsMeathookModule(m)))
                    throw new InvalidOperationException("Saved Meathook module is no longer loaded");
                // Older development snapshots used ContainsCode alone, which packed PE
                // headers can also set on mutable data. Retain only actual executable
                // code, upstream's explicit undo targets and its main-loop callback.
                var recorded = new HashSet<IntPtr>();
                IntPtr link = _game.ReadPointer(_module + PatchHead);
                for(int i = 0; link != IntPtr.Zero && i < 4096; i++) {
                    byte[] record = _game.ReadBytes(link, 40) ?? throw new InvalidOperationException("Cannot read owned patch targets");
                    recorded.Add(new IntPtr(BitConverter.ToInt64(record, 32))); link = new IntPtr(BitConverter.ToInt64(record, 16));
                }
                recorded.Add(_game.ReadPointer(_module + FramePointerGlobal));
                using var file = File.OpenRead(game.MainModule!.FileName); using var pe = new PEReader(file);
                var executable = pe.PEHeaders.SectionHeaders.Where(IsStableCode).ToArray();
                long image = game.MainModule.BaseAddress.ToInt64();
                int removed = _patches.RemoveAll(p => !recorded.Contains(p.Address) && !executable.Any(s => p.Address.ToInt64() >= image + s.VirtualAddress &&
                    p.Address.ToInt64() + p.Before.Length <= image + s.VirtualAddress + s.VirtualSize));
                if(removed > 0) { _savedState = IntPtr.Zero; SaveState(); }
            } else {
                var startup = FindStartupModule();
                if(startup != null) AdoptStartupModule(startup);
            }
        }

        private static bool IsMeathookModule(ProcessModule module) {
            if(module.ModuleName.Equals("DESRU.Meathook.Live.dll", StringComparison.OrdinalIgnoreCase)) return true;
            return module.ModuleName.Equals("XINPUT1_3.dll", StringComparison.OrdinalIgnoreCase) &&
                Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(module.FileName))).Equals(OriginalHash, StringComparison.OrdinalIgnoreCase);
        }

        private ProcessModule? FindStartupModule() {
            string directory = Path.GetDirectoryName(_game.MainModule!.FileName)!;
            return _game.Modules.Cast<ProcessModule>().FirstOrDefault(m => m.ModuleName.Equals("XINPUT1_3.dll", StringComparison.OrdinalIgnoreCase) &&
                Path.GetDirectoryName(m.FileName)!.Equals(directory, StringComparison.OrdinalIgnoreCase) && IsMeathookModule(m));
        }

        private void PrepareDisabledCommands(IReadOnlyDictionary<string, IntPtr> originalCommands) {
            _noOp = _game.AllocateMemory(4096);
            if(_noOp == IntPtr.Zero || !_game.WriteBytes(_noOp, new byte[] { 0x31, 0xc0, 0xc3 }) || !_game.VirtualProtect(_noOp, 4096, MemPageProtect.PAGE_EXECUTE_READ))
                throw new InvalidOperationException("Cannot prepare disabled command handlers");
            FlushInstructionCache(_game.Handle, _noOp, (UIntPtr) 3);
            foreach(var command in Commands()) {
                if(command.Function.ToInt64() < _module.ToInt64() || command.Function.ToInt64() >= _module.ToInt64() + 0x75000) continue;
                IntPtr disabled = originalCommands.TryGetValue(command.Name, out var previous) ? previous : _noOp;
                _commands.Add(new(command.Slot, BitConverter.GetBytes(disabled.ToInt64()), BitConverter.GetBytes(command.Function.ToInt64())));
            }
        }

        private void AdoptStartupModule(ProcessModule module) {
            _module = module.BaseAddress;
            _frameTarget = _game.ReadPointer(_module + FramePointerGlobal);
            IntPtr original = _game.ReadPointer(_module + 0x68ba8);
            long image = _game.MainModule!.BaseAddress.ToInt64();
            int imageSize = _game.MainModule.ModuleMemorySize;
            if(original.ToInt64() < image || original.ToInt64() >= image + imageSize) throw new InvalidOperationException("Cannot resolve original game frame callback");
            _frameOriginal = BitConverter.GetBytes(original.ToInt64());
            CollectPatches(new());
            // The original proxy does not record its filesystem detour. Identify only
            // its exact mov-absolute/jump entry through its verified scanned pointers.
            byte[] globals = _game.ReadBytes(_module + 0x52000, 0x17378) ?? throw new InvalidOperationException("Cannot read Meathook scanner globals");
            using var file = File.OpenRead(_game.MainModule.FileName); using var pe = new PEReader(file, PEStreamOptions.LeaveOpen);
            var seen = new HashSet<long>();
            for(int i = 0; i + 8 <= globals.Length; i += 8) {
                long address = BitConverter.ToInt64(globals, i);
                if(address < image || address >= image + imageSize || !seen.Add(address) || _patches.Any(p => p.Address.ToInt64() == address)) continue;
                int rva = checked((int) (address - image));
                var sections = pe.PEHeaders.SectionHeaders.Where(s => IsStableCode(s) && rva >= s.VirtualAddress && rva + 12 <= s.VirtualAddress + s.SizeOfRawData).ToArray();
                if(sections.Length != 1) continue;
                byte[]? after = _game.ReadBytes(new IntPtr(address), 12);
                if(after == null || after[0] != 0x48 || after[1] != 0xb8 || after[10] != 0xff || after[11] != 0xe0) continue;
                long destination = BitConverter.ToInt64(after, 2);
                if(destination < _module.ToInt64() || destination >= _module.ToInt64() + module.ModuleMemorySize) continue;
                file.Position = rva - sections[0].VirtualAddress + sections[0].PointerToRawData;
                byte[] before = new byte[12]; if(file.Read(before) != before.Length) throw new InvalidOperationException("Cannot read original filesystem entry");
                _patches.Add(new(new IntPtr(address), before, after));
            }
            PrepareDisabledCommands(new Dictionary<string, IntPtr>());
            if(_commands.Count == 0 || _patches.Count < 3) throw new InvalidOperationException("The startup Meathook has not completed initialization");
            Active = true; SaveState();
        }

        private void SaveState() {
            if(_savedState != IntPtr.Zero) {
                if(!_game.WriteBytes(_savedState + 16, BitConverter.GetBytes(Active ? 1 : 0))) throw new InvalidOperationException("Cannot preserve Meathook active state");
                return;
            }
            using var stream = new MemoryStream(); using var writer = new BinaryWriter(stream);
            writer.Write(Encoding.ASCII.GetBytes("DESRUMH1")); writer.Write(_module.ToInt64()); writer.Write(Active ? 1 : 0); writer.Write(0);
            foreach(var list in new[] { _patches, _commands }) {
                writer.Write(list.Count);
                foreach(var patch in list) { writer.Write(patch.Address.ToInt64()); writer.Write(patch.Before.Length); writer.Write(patch.Before); writer.Write(patch.After); }
            }
            byte[] data = stream.ToArray(); BitConverter.GetBytes(data.Length).CopyTo(data, 20);
            IntPtr allocation = _game.AllocateMemory(data.Length);
            if(allocation == IntPtr.Zero || !_game.WriteBytes(allocation, data) || !_game.WriteBytes(_hud.State + 128, BitConverter.GetBytes(allocation.ToInt64())))
                throw new InvalidOperationException("Cannot preserve Meathook reconnect state");
            _savedState = allocation;
        }

        internal static byte[] PrepareImage(byte[] original) {
            if(!Convert.ToHexString(SHA256.HashData(original)).Equals(OriginalHash, StringComparison.OrdinalIgnoreCase))
                throw new NotSupportedException("Live Meathook requires the verified bundled DLL; another DLL was left unchanged");
            byte[] result = (byte[]) original.Clone();
            using var stream = new MemoryStream(result);
            using var pe = new PEReader(stream);
            int Offset(int rva) {
                var section = pe.PEHeaders.SectionHeaders.Single(s => rva >= s.VirtualAddress && rva < s.VirtualAddress + s.SizeOfRawData);
                return rva - section.VirtualAddress + section.PointerToRawData;
            }
            // DllMain has already initialized upstream's CPU dispatcher and game image.
            int start = Offset(0xf128);
            result[start] = 0xe9; BitConverter.GetBytes(0xf179 - 0xf128 - 5).CopyTo(result, start + 1);
            // Late initialization must not call the game's startup initializer a second time.
            Enumerable.Repeat((byte) 0x90, 5).ToArray().CopyTo(result, Offset(0xf2c2));
            new byte[] { 0x31, 0xc0, 0x90, 0x90, 0x90, 0x90 }.CopyTo(result, Offset(0xf2cd));
            return result;
        }

        private IReadOnlyList<(string Name, IntPtr Slot, IntPtr Function)> Commands() {
            IntPtr system = _game.ReadPointer(_game.MainModule!.BaseAddress + _layout.CommandGlobal);
            IntPtr control = _game.ReadPointer(system + 16);
            byte[] header = _game.ReadBytes(control, 16) ?? throw new InvalidOperationException("Cannot read command registry");
            IntPtr array = new(BitConverter.ToInt64(header)); int count = BitConverter.ToInt32(header, 8);
            if(count < 1 || count > 65536) throw new InvalidOperationException("Invalid command registry");
            byte[] pointers = _game.ReadBytes(array, count * 8) ?? throw new InvalidOperationException("Cannot read command registry pointers");
            var results = new List<(string, IntPtr, IntPtr)>();
            for(int i = 0; i < count; i++) {
                var entry = new IntPtr(BitConverter.ToInt64(pointers, i * 8));
                byte[]? bytes = _game.ReadBytes(entry, 40); if(bytes == null) continue;
                string? name = _game.ReadString(new IntPtr(BitConverter.ToInt64(bytes)), ReadStringType.ASCII, 96);
                if(!string.IsNullOrEmpty(name)) results.Add((name, entry + 8, new IntPtr(BitConverter.ToInt64(bytes, 8))));
            }
            return results;
        }

        private static bool IsStableCode(SectionHeader section) =>
            (section.SectionCharacteristics & SectionCharacteristics.ContainsCode) != 0 &&
            (section.SectionCharacteristics & SectionCharacteristics.MemExecute) != 0 &&
            (section.SectionCharacteristics & SectionCharacteristics.MemWrite) == 0 && section.VirtualSize > 0 && section.VirtualSize < 64 * 1024 * 1024;

        private List<(IntPtr Address, byte[] Bytes)> SnapshotCode() {
            using var stream = File.OpenRead(_game.MainModule!.FileName);
            using var pe = new PEReader(stream);
            return pe.PEHeaders.SectionHeaders.Where(IsStableCode)
                .Select(s => (_game.MainModule.BaseAddress + s.VirtualAddress, _game.ReadBytes(_game.MainModule.BaseAddress + s.VirtualAddress, s.VirtualSize) ?? throw new InvalidOperationException("Cannot snapshot game code"))).ToList();
        }

        private void CollectPatches(List<(IntPtr Address, byte[] Bytes)> codeBefore) {
            var visited = new HashSet<IntPtr>();
            IntPtr link = _game.ReadPointer(_module + PatchHead);
            var reverse = new List<Patch>();
            while(link != IntPtr.Zero) {
                if(!visited.Add(link) || visited.Count > 4096) throw new InvalidOperationException("Invalid Meathook patch chain");
                byte[] record = _game.ReadBytes(link, 40) ?? throw new InvalidOperationException("Cannot read Meathook patch");
                int count = checked((int) BitConverter.ToInt64(record, 24));
                if(count < 1 || count > 65536) throw new InvalidOperationException("Invalid Meathook patch length");
                IntPtr address = new(BitConverter.ToInt64(record, 32));
                var before = _game.ReadBytes(new IntPtr(BitConverter.ToInt64(record)), count);
                var after = _game.ReadBytes(new IntPtr(BitConverter.ToInt64(record, 8)), count);
                if(before == null || after == null) throw new InvalidOperationException("Cannot read Meathook saved bytes");
                byte[]? actual = _game.ReadBytes(address, count);
                // The proxy's temporary GetSystemInfo hook has already restored itself.
                if(actual != null && actual.SequenceEqual(after)) reverse.Add(new Patch(address, before, after));
                else if(actual == null || !actual.SequenceEqual(before)) throw new InvalidOperationException("An upstream Meathook patch no longer matches its saved state");
                link = new IntPtr(BitConverter.ToInt64(record, 16));
            }
            reverse.Reverse(); _patches.AddRange(reverse);
            if(_frameOriginal != null) _patches.Add(new(_frameTarget, _frameOriginal, _game.ReadBytes(_frameTarget, 8)!));
            // Upstream has one filesystem detour written without its undo list. Capture
            // only newly changed code bytes, excluding the already recorded patch ranges.
            foreach(var section in codeBefore) {
                byte[] after = _game.ReadBytes(section.Address, section.Bytes.Length) ?? throw new InvalidOperationException("Cannot compare Meathook code");
                for(int i = 0; i < after.Length; i++) {
                    if(after[i] == section.Bytes[i] || _patches.Any(p => section.Address.ToInt64() + i >= p.Address.ToInt64() && section.Address.ToInt64() + i < p.Address.ToInt64() + p.After.Length)) continue;
                    int begin = i;
                    while(i + 1 < after.Length && after[i + 1] != section.Bytes[i + 1]) i++;
                    _patches.Add(new(section.Address + begin, section.Bytes.AsSpan(begin, i - begin + 1).ToArray(), after.AsSpan(begin, i - begin + 1).ToArray()));
                }
            }
        }

        internal async Task EnableAsync(string source) {
            if(Active) return;
            if(_module != IntPtr.Zero) {
                if(_savedState == IntPtr.Zero) throw new InvalidOperationException("Meathook initialization has not completed; its handlers were left allocated");
                Apply(true); Active = true; SaveState(); return;
            }
            var existing = _game.Modules.Cast<ProcessModule>().FirstOrDefault(m => m.ModuleName.Equals("DESRU.Meathook.Live.dll", StringComparison.OrdinalIgnoreCase));
            if(existing != null && _game.ReadPointer(_hud.State + 136) != existing.BaseAddress && _game.ReadPointer(_hud.State + 40) != existing.BaseAddress + 0x383c0)
                throw new NotSupportedException("A live Meathook module from another session is present; reconnect its state before enabling");
            // Do not load a second copy over an already active startup proxy.
            string gameDirectory = Path.GetDirectoryName(_game.MainModule!.FileName)!;
            if(_game.Modules.Cast<ProcessModule>().Any(m => m.ModuleName.Equals("XINPUT1_3.dll", StringComparison.OrdinalIgnoreCase) && Path.GetDirectoryName(m.FileName)!.Equals(gameDirectory, StringComparison.OrdinalIgnoreCase)))
                throw new NotSupportedException("The startup Meathook is already loaded. Use a game session without the startup proxy for live control");
            byte[] patched = PrepareImage(File.ReadAllBytes(source));
            string target = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(source))!, "DESRU.Meathook.Live.dll");
            if(existing == null) File.WriteAllBytes(target, patched);
            var originalCommands = Commands().ToDictionary(c => c.Name, c => c.Function, StringComparer.OrdinalIgnoreCase);
            var before = SnapshotCode();
            byte[] path = Encoding.Unicode.GetBytes(target + '\0');
            IntPtr pathBuffer = _game.AllocateMemory(path.Length);
            if(pathBuffer == IntPtr.Zero || !_game.WriteBytes(pathBuffer, path)) throw new InvalidOperationException("Cannot publish Meathook path");
            bool loaded = false;
            try {
                if(existing != null) { _module = existing.BaseAddress; loaded = true; }
                else {
                IntPtr localLoad = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW");
                using var local = Process.GetCurrentProcess();
                var localModule = local.Modules.Cast<ProcessModule>().Single(m => localLoad.ToInt64() >= m.BaseAddress.ToInt64() && localLoad.ToInt64() < m.BaseAddress.ToInt64() + m.ModuleMemorySize);
                var remoteModule = _game.Modules.Cast<ProcessModule>().Single(m => m.ModuleName.Equals(localModule.ModuleName, StringComparison.OrdinalIgnoreCase));
                await _hud.InvokeAsync(remoteModule.BaseAddress + checked((int) (localLoad.ToInt64() - localModule.BaseAddress.ToInt64())), pathBuffer);
                _module = _game.ReadPointer(_hud.State + 120); loaded = true;
                if(_module == IntPtr.Zero) throw new InvalidOperationException("The game could not load Meathook");
                _game.Refresh();
                }
                _game.WriteBytes(_hud.State + 136, BitConverter.GetBytes(_module.ToInt64()));
            } finally { if(loaded) _game.FreeMemory(pathBuffer); }
            // Upgrade an already loaded, not-yet-initialized late-entry image as well.
            if(!_game.VirtualProtect(_module + 0xf2c2, 5, MemPageProtect.PAGE_EXECUTE_READWRITE, out var oldProtection)) throw new InvalidOperationException("Cannot select late scanner entry");
            try { _game.WriteBytes(_module + 0xf2c2, Enumerable.Repeat((byte) 0x90, 5).ToArray()); }
            finally { _game.VirtualProtect(_module + 0xf2c2, 5, oldProtection); }
            FlushInstructionCache(_game.Handle, _module + 0xf2c2, (UIntPtr) 5);
            await _hud.InvokeAsync(_module + 0x383c0); // upstream critical scanners
            await _hud.InvokeAsync(_module + 0x38410); // upstream phase 2, before saving its resolved callback
            _frameTarget = _game.ReadPointer(_module + FramePointerGlobal);
            _frameOriginal = _game.ReadBytes(_frameTarget, 8) ?? throw new InvalidOperationException("Cannot save game frame callback");
            await _hud.InvokeAsync(_module + 0xf290); // phase 2 + installation, excluding game startup
            Active = true; // installation has executed even if later bookkeeping fails
            // First main-loop tick performs upstream phase 3 and installs the GUI bridge.
            await Task.Delay(500);
            CollectPatches(before);
            PrepareDisabledCommands(originalCommands);
            if(!_commands.Any() || !_patches.Any()) throw new InvalidOperationException("Meathook initialization has not completed");
            Active = true;
            SaveState();
        }

        internal void Disable() {
            if(!Active) return;
            if(_savedState == IntPtr.Zero) throw new InvalidOperationException("Meathook initialization is incomplete; no complete restore snapshot exists");
            Apply(false); Active = false; SaveState();
        }

        internal async Task ClearPlayerCheatsAsync(IntPtr player) {
            if(player == IntPtr.Zero) return;
            if(_layout.PlayerCheatSetters.Length != 3) throw new NotSupportedException("Cannot resolve player cheat reset functions");
            foreach(int setter in _layout.PlayerCheatSetters) await _hud.InvokeAsync(_game.MainModule!.BaseAddress + setter, player, IntPtr.Zero);
        }

        internal static IntPtr UnwrapHud(Process game, IntPtr slot, IntPtr current) {
            var module = game.Modules.Cast<ProcessModule>().FirstOrDefault(m => current.ToInt64() >= m.BaseAddress.ToInt64() && current.ToInt64() < m.BaseAddress.ToInt64() + m.ModuleMemorySize && IsMeathookModule(m));
            if(module == null || current.ToInt64() < module.BaseAddress.ToInt64() || current.ToInt64() >= module.BaseAddress.ToInt64() + module.ModuleMemorySize) return slot;
            IntPtr patch = game.ReadPointer(module.BaseAddress + PatchHead);
            for(int i = 0; patch != IntPtr.Zero && i < 4096; i++) {
                byte[] record = game.ReadBytes(patch, 40) ?? throw new InvalidOperationException("Cannot read Meathook HUD chain");
                if(new IntPtr(BitConverter.ToInt64(record, 32)) == slot && BitConverter.ToInt64(record, 24) == 8) {
                    IntPtr original = game.ReadPointer(new IntPtr(BitConverter.ToInt64(record)));
                    byte[] data = game.ReadBytes(module.BaseAddress + 0x52000, 0x17378) ?? throw new InvalidOperationException("Cannot read Meathook HUD bridge");
                    byte[] value = BitConverter.GetBytes(original.ToInt64());
                    var matches = Enumerable.Range(0, data.Length / 8).Where(n => data.AsSpan(n * 8, 8).SequenceEqual(value)).ToArray();
                    if(matches.Length != 1) throw new InvalidOperationException("Cannot uniquely reconnect the Meathook HUD bridge");
                    return module.BaseAddress + 0x52000 + matches[0] * 8;
                }
                patch = new IntPtr(BitConverter.ToInt64(record, 16));
            }
            throw new InvalidOperationException("Cannot reconnect the Meathook HUD chain");
        }

        internal static void SynchronizeHudBridge(Process game, IntPtr actualSlot, IntPtr bridgeSlot, IntPtr renderer) {
            if(actualSlot == bridgeSlot) return;
            IntPtr wrapper = game.ReadPointer(actualSlot);
            var module = game.Modules.Cast<ProcessModule>().FirstOrDefault(m => wrapper.ToInt64() >= m.BaseAddress.ToInt64() && wrapper.ToInt64() < m.BaseAddress.ToInt64() + m.ModuleMemorySize && IsMeathookModule(m));
            if(module == null) throw new NotSupportedException("Cannot update an unknown HUD bridge");
            IntPtr link = game.ReadPointer(module.BaseAddress + PatchHead);
            for(int i = 0; link != IntPtr.Zero && i < 4096; i++) {
                byte[] record = game.ReadBytes(link, 40) ?? throw new InvalidOperationException("Cannot update Meathook HUD ownership");
                if(new IntPtr(BitConverter.ToInt64(record, 32)) == actualSlot && BitConverter.ToInt64(record, 24) == 8) {
                    if(!game.WriteBytes(new IntPtr(BitConverter.ToInt64(record)), BitConverter.GetBytes(renderer.ToInt64()))) throw new InvalidOperationException("Cannot preserve DESRU across Meathook switching");
                    return;
                }
                link = new IntPtr(BitConverter.ToInt64(record, 16));
            }
            throw new InvalidOperationException("Missing Meathook HUD bridge ownership");
        }

        private void Apply(bool enable) {
            var patches = enable ? _patches.Concat(_commands).ToArray() : _commands.AsEnumerable().Reverse().Concat(_patches.AsEnumerable().Reverse()).ToArray();
            if(WinAPI.NtSuspendProcess(_game.Handle) != IntPtr.Zero) throw new InvalidOperationException("Cannot pause game while switching Meathook");
            var applied = new List<Patch>();
            try {
                // Validate and apply in order: upstream may have layered patches on one address.
                foreach(var patch in patches) {
                    byte[] expected = enable ? patch.Before : patch.After, desired = enable ? patch.After : patch.Before;
                    byte[] actual = _game.ReadBytes(patch.Address, expected.Length) ?? Array.Empty<byte>();
                    if(!actual.SequenceEqual(expected))
                        throw new InvalidOperationException($"Meathook handler mismatch at {patch.Address.ToInt64():X}: expected {Convert.ToHexString(expected)}, read {Convert.ToHexString(actual)}; switching stopped");
                    if(!_game.VirtualProtect(patch.Address, desired.Length, MemPageProtect.PAGE_EXECUTE_READWRITE, out var protection)) throw new InvalidOperationException("Cannot protect Meathook handler");
                    try { if(!_game.WriteBytes(patch.Address, desired)) throw new InvalidOperationException("Cannot switch Meathook handler"); }
                    finally { _game.VirtualProtect(patch.Address, desired.Length, protection); }
                    applied.Add(patch);
                }
                FlushInstructionCache(_game.Handle, IntPtr.Zero, UIntPtr.Zero);
            } catch {
                foreach(var patch in applied.AsEnumerable().Reverse()) {
                    byte[] rollback = enable ? patch.Before : patch.After;
                    _game.VirtualProtect(patch.Address, rollback.Length, MemPageProtect.PAGE_EXECUTE_READWRITE, out var protection);
                    _game.WriteBytes(patch.Address, rollback); _game.VirtualProtect(patch.Address, rollback.Length, protection);
                }
                FlushInstructionCache(_game.Handle, IntPtr.Zero, UIntPtr.Zero);
                throw;
            } finally { WinAPI.NtResumeProcess(_game.Handle); }
            // DLL/code and registered callback storage stay allocated until game exit.
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandle(string name);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("kernel32.dll")] private static extern bool FlushInstructionCache(IntPtr process, IntPtr address, UIntPtr length);
    }
}
