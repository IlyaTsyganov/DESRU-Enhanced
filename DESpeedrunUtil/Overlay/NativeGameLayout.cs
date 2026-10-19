using System.Buffers.Binary;
using System.IO.MemoryMappedFiles;
using System.Reflection.PortableExecutable;
using System.Text;

namespace DESpeedrunUtil.Overlay {
    // Resolve a coherent layout from the executable itself. No offsets are guessed from a version label.
    internal sealed record NativeGameLayout(int ImageSize, int HudSlot, int Render, int Draw, int Width, int Height,
        int Print, int CommandGlobal, int GlyphWidth, int GlyphHeight, int GroundPlaneFromVelocity) {
        public NativeTintLayout? Tint { get; init; }
        public string? TintFailure { get; init; }
        public NativeCvarLayout? Cvars { get; init; }
        public int[] PlayerCheatSetters { get; init; } = Array.Empty<int>();
        public NativePlayerCheatLayout? PlayerCheats { get; init; }
        private static readonly Dictionary<string, NativeGameLayout> Cache = new();
        internal static NativeGameLayout Resolve(string path) {
            var file = new FileInfo(path);
            string key = file.FullName + ":" + file.Length + ":" + file.LastWriteTimeUtc.Ticks;
            lock(Cache) {
                if(Cache.TryGetValue(key, out var cached)) return cached;
                using var image = new Executable(path);
                return Cache[key] = image.Resolve();
            }
        }

        private sealed unsafe class Executable : IDisposable {
            private readonly MemoryMappedFile _mapping;
            private readonly MemoryMappedViewAccessor _view;
            private readonly System.Collections.Immutable.ImmutableArray<SectionHeader> _sections;
            private byte* _pointer;
            private readonly int _length, _imageSize;
            private readonly long _base;
            private readonly Dictionary<string, List<(int Size, Dictionary<string, (int Offset, int Size)> Fields)>> _metadata = new();
            private ReadOnlySpan<byte> Bytes => new(_pointer, _length);

            internal Executable(string path) {
                using var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                using var pe = new PEReader(stream, PEStreamOptions.LeaveOpen);
                var header = pe.PEHeaders.PEHeader ?? throw new NotSupportedException("Missing game PE header");
                if(header.Magic != PEMagic.PE32Plus || stream.Length > int.MaxValue) throw new NotSupportedException("Expected a bounded x64 game executable");
                _sections = pe.PEHeaders.SectionHeaders;
                _imageSize = header.SizeOfImage; _base = (long) header.ImageBase; _length = (int) stream.Length;
                _mapping = MemoryMappedFile.CreateFromFile(stream, null, 0, MemoryMappedFileAccess.Read, HandleInheritability.None, true);
                _view = _mapping.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read);
                _view.SafeMemoryMappedViewHandle.AcquirePointer(ref _pointer);
                _pointer += _view.PointerOffset;
            }

            private int Rva(int offset) {
                foreach(var section in _sections)
                    if(offset >= section.PointerToRawData && (long) offset < (long) section.PointerToRawData + section.SizeOfRawData)
                        return checked(offset - section.PointerToRawData + section.VirtualAddress);
                throw new NotSupportedException("Game reference is outside PE sections");
            }
            private int Offset(int rva) {
                foreach(var section in _sections)
                    if(rva >= section.VirtualAddress && (long) rva < (long) section.VirtualAddress + section.SizeOfRawData)
                        return checked(rva - section.VirtualAddress + section.PointerToRawData);
                throw new NotSupportedException("Game metadata points outside mapped file data");
            }
            private int Int(int offset) => BinaryPrimitives.ReadInt32LittleEndian(Bytes.Slice(offset, 4));
            private long Long(int offset) => BinaryPrimitives.ReadInt64LittleEndian(Bytes.Slice(offset, 8));
            private int Relative(int offset) => checked(Rva(offset) + 4 + Int(offset));
            private bool InImage(int rva) => rva > 0 && rva < _imageSize;
            private bool IsCode(int rva) => _sections.Any(s => rva >= s.VirtualAddress && (long) rva < (long) s.VirtualAddress + s.VirtualSize &&
                (s.SectionCharacteristics & SectionCharacteristics.MemExecute) != 0);
            private List<int> Find(byte[] needle) {
                var found = new List<int>();
                for(int offset = 0; offset <= _length - needle.Length;) {
                    int relative = Bytes[offset..].IndexOf(needle);
                    if(relative < 0) break;
                    offset += relative; found.Add(offset++);
                }
                return found;
            }
            private List<int> Pattern(string pattern) {
                var parts = pattern.Split(' ');
                var bytes = parts.Select(p => p == "??" ? (byte) 0 : Convert.ToByte(p, 16)).ToArray();
                int best = 0, count = 0, start = 0;
                for(int i = 0; i < parts.Length;) {
                    if(parts[i] == "??") { i++; continue; }
                    int at = i; while(i < parts.Length && parts[i] != "??") i++;
                    if(i - at > count) { count = i - at; start = at; }
                }
                best = start;
                var result = new List<int>();
                foreach(int hit in Find(bytes.AsSpan(best, count).ToArray())) {
                    int at = hit - best;
                    if(at < 0 || at > _length - bytes.Length) continue;
                    bool match = true;
                    for(int i = 0; i < bytes.Length; i++) if(parts[i] != "??" && Bytes[at + i] != bytes[i]) { match = false; break; }
                    if(match) result.Add(at);
                }
                return result;
            }
            private static int Unique(IEnumerable<int> values, string label) {
                var found = values.Distinct().ToArray();
                return found.Length == 1 ? found[0] : throw new NotSupportedException($"Cannot uniquely resolve {label}: {found.Length} candidates");
            }
            private int Vtable(string name) {
                var tables = new List<int>();
                foreach(int text in Find(Encoding.ASCII.GetBytes(name + "\0"))) {
                    int descriptor = Rva(text - 16);
                    foreach(int reference in Find(BitConverter.GetBytes(descriptor))) {
                        int locator = reference - 12;
                        if(locator < 0 || locator > _length - 24) continue;
                        if(Int(locator) != 1 || Int(locator + 4) != 0 || Int(locator + 8) != 0) continue;
                        int locatorRva;
                        try { locatorRva = Rva(locator); } catch(NotSupportedException) { continue; }
                        if(Int(locator + 20) != locatorRva) continue;
                        foreach(int pointer in Find(BitConverter.GetBytes(_base + locatorRva))) {
                            if((Rva(pointer) & 7) == 0) tables.Add(Rva(pointer + 8));
                        }
                    }
                }
                return Unique(tables, name + " primary RTTI vtable");
            }
            private int GroundOffset() {
                var results = new List<int>();
                foreach(int name in Find(Encoding.ASCII.GetBytes("playerPState_t\0"))) {
                    foreach(int reference in Find(BitConverter.GetBytes(_base + Rva(name)))) {
                        try {
                            if(reference > _length - 40 || Int(reference + 16) < 1 || Int(reference + 16) > 4096) continue;
                            int fields = Offset(checked((int) (Long(reference + 32) - _base))), velocity = -1, ground = -1;
                            for(int i = 0; i < 256; i++) {
                                int field = checked(fields + i * 72);
                                long pointer = Long(field + 16); if(pointer == 0) break;
                                int fieldName = Offset(checked((int) (pointer - _base)));
                                var text = Bytes.Slice(fieldName, Math.Min(64, _length - fieldName));
                                if(text.StartsWith(Encoding.ASCII.GetBytes("velocity\0")) && Int(field + 28) == 12) velocity = Int(field + 24);
                                if(text.StartsWith(Encoding.ASCII.GetBytes("groundPlane\0")) && Int(field + 28) == 1) ground = Int(field + 24);
                            }
                            if(velocity >= 0 && ground > velocity && ground - velocity < 1024) results.Add(ground - velocity);
                        } catch(Exception e) when(e is NotSupportedException or ArgumentOutOfRangeException or OverflowException) { }
                    }
                }
                return results.Count == 0 ? 0 : Unique(results, "groundPlane offset");
            }
            private string Text(long pointer) {
                int offset = Offset(checked((int) (pointer - _base)));
                var text = Bytes.Slice(offset, Math.Min(256, _length - offset));
                int end = text.IndexOf((byte) 0);
                if(end < 0) throw new NotSupportedException("Unterminated metadata name");
                return Encoding.ASCII.GetString(text[..end]);
            }
            private List<(int Size, Dictionary<string, (int Offset, int Size)> Fields)> Metadata(string type) {
                if(_metadata.TryGetValue(type, out var cached)) return cached;
                var found = new List<(int, Dictionary<string, (int, int)>)>();
                foreach(int name in Find(Encoding.ASCII.GetBytes(type + "\0"))) {
                    foreach(int reference in Find(BitConverter.GetBytes(_base + Rva(name)))) {
                        try {
                            int size = Int(reference + 16);
                            if(size <= 0 || size > 0x1000000) continue;
                            int fields = Offset(checked((int) (Long(reference + 32) - _base)));
                            var values = new Dictionary<string, (int, int)>();
                            bool complete = false;
                            for(int i = 0; i < 4096; i++) {
                                int at = checked(fields + i * 72);
                                long pointer = Long(at + 16);
                                if(pointer == 0) { complete = true; break; }
                                string field = Text(pointer);
                                int offset = Int(at + 24), length = Int(at + 28);
                                if(offset >= 0 && length > 0 && (long) offset + length <= size) values[field] = (offset, length);
                            }
                            if(complete && values.Count > 0) found.Add((size, values));
                        } catch(Exception e) when(e is NotSupportedException or ArgumentOutOfRangeException or OverflowException) { }
                    }
                }
                return _metadata[type] = found;
            }
            private int? TryField(string type, string field) {
                var offsets = Metadata(type).Where(m => m.Fields.ContainsKey(field)).Select(m => m.Fields[field].Offset).ToArray();
                return offsets.Length == 0 ? null : Unique(offsets, type + "." + field);
            }
            private int Field(string type, string field) => TryField(type, field) ?? throw new NotSupportedException("Missing metadata: " + type + "." + field);
            private int PropertyAccessor(string type, string field, bool setter) {
                var found = new List<int>();
                foreach(int name in Find(Encoding.ASCII.GetBytes(type + "\0")))
                    foreach(int reference in Find(BitConverter.GetBytes(_base + Rva(name)))) {
                        try {
                            int fields = Offset(checked((int) (Long(reference + 32) - _base)));
                            for(int i = 0; i < 4096; i++) {
                                int at = fields + i * 72;
                                if(Long(at + 16) == 0) break;
                                if(Text(Long(at + 16)) != field) continue;
                                int accessor = checked((int) (Long(at + (setter ? 56 : 48)) - _base));
                                if(IsCode(accessor)) found.Add(accessor);
                            }
                        } catch(Exception e) when(e is NotSupportedException or ArgumentOutOfRangeException or OverflowException) { }
                    }
                return Unique(found, type + "." + field + (setter ? " setter" : " getter"));
            }
            private NativePlayerCheatFlag PlayerCheatFlag(string field) => NativePlayerCheatFlag.Decode(Bytes.Slice(Offset(PropertyAccessor("idPlayer", field, false)), 16));
            private int EnumValue(string type, string value) {
                var found = new List<int>();
                foreach(int name in Find(Encoding.ASCII.GetBytes(type + "\0"))) {
                    foreach(int reference in Find(BitConverter.GetBytes(_base + Rva(name)))) {
                        try {
                            int values = Offset(checked((int) (Long(reference + 24) - _base)));
                            for(int i = 0; i < 4096; i++) {
                                long pointer = Long(values + i * 16);
                                if(pointer == 0) break;
                                if(Text(pointer) == value) { found.Add(checked((int) Long(values + i * 16 + 8))); break; }
                            }
                        } catch(Exception e) when(e is NotSupportedException or ArgumentOutOfRangeException or OverflowException) { }
                    }
                }
                return Unique(found, type + "::" + value);
            }
            private NativeTintLayout ResolveTint() {
                int hud = Vtable(".?AVidHUD_LowWarning@@");
                int update = checked((int) (Long(Offset(hud + 9 * 8)) - _base));
                int hook = Unique(Pattern("48 8B 43 10 48 85 C0 74 ?? F3 0F 11").Select(Rva).Where(rva => rva >= update && rva < update + 0x300), "HUD alpha hook");
                if(Field("idUIElement", "swfObject") + Field("idSWF_GameObject", "swf") != 0x10 ||
                    Field("idSWFSpriteInstance", "name") != 0x18 || Field("idSWFSpriteInstance", "displayList") != 0x70 ||
                    Unique(Metadata("swfDisplayEntry_t").Select(m => m.Size), "SWF entry size") != 0x40 ||
                    Unique(Metadata("idView::screenVignette_t").Select(m => m.Size), "vignette entry size") != 0x50 ||
                    Field("idView::screenVignette_t", "actionGroupHandle") != 0 || Field("idView::screenVignette_t", "declActionIdx") != 8)
                    throw new NotSupportedException("Unsupported SWF or screen-vignette layout");
                var legacy = Pattern("F3 41 0F 10 46 10 0F 2E C6 7A 02 74 ?? 48 8B 74 24 ?? FF 86 ?? ?? ?? ??");
                var modern = Pattern("F3 43 0F 10 84 BE ?? ?? ?? ?? 0F 2E C7 7A 02 74 07 41 FF 86 ?? ?? ?? ??");
                int site = Unique(legacy.Concat(modern), "screen-vignette alpha hook");
                bool indexed = modern.Contains(site);
                int actions = TryField("idDeclFX", "events") ?? Field("idDeclFX", "editEvents");
                return new NativeTintLayout(hook, hud, Field("idSWF", "mainspriteInstance"), Rva(site), indexed,
                    Field("idView", "screenVignettesBuffer"), indexed ? Int(site + 6) : 0x10,
                    Vtable(".?AVidPlayer@@"), Vtable(".?AVidDeclFX@@"), Field("idPlayer", "screenFXDecl"), Field("idPlayer", "screenFX"),
                    Field("idPlayer", "damageFeedbackComponent") + Field("idDamageFeedbackComponent", "curHealthScreenFXSlot"), actions,
                    Unique(Metadata("idFXSingleAction").Select(m => m.Size), "FX action size"), Field("idFXSingleAction", "type"),
                    Field("idFXSingleAction", "startCondition"), EnumValue("fxActionType_t", "FX_SCREEN_VIGNETTE"),
                    EnumValue("fxCondition_t", "FX_HEALTH_LOW"), EnumValue("fxCondition_t", "FX_HEALTH_REALLY_LOW"));
            }
            internal NativeGameLayout Resolve() {
                int slot = Vtable(".?AVidDebugHUDLocal@@") + 8;
                int render = checked((int) (Long(Offset(slot)) - _base));
                const string drawPattern = "89 4C 24 28 41 8B C0 41 2B C1 48 8D 8D ?? 01 00 00 89 44 24 20 E8 ?? ?? ?? ?? F3 44 0F 11 64 24 30 4C 8B C8 C6 44 24 28 00 41 0F 28 D0 41 0F 28 CD ?? 89 ?? 24 20 ?? 8B ?? E8";
                int draw = Relative(Unique(Pattern(drawPattern), "DrawString reference") + drawPattern.Split(' ').Length);
                int dimensions = Unique(Pattern("48 8B 4B 40 E8 ?? ?? ?? ?? F3 0F 2C C8 8B C7 39 0D ?? ?? ?? ?? 0F 4C 0D ?? ?? ?? ?? 85 C9 0F 4F C1 89 05 ?? ?? ?? ?? 48 8B 4B 40 E8"), "virtual dimensions");
                const string printPattern = "84 C9 74 2C 48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 48 83 EC 28 4C 8B C2 4C 8D 4C 24 40 BA 05 00 00 00 8D 4A FC E8";
                int print = Relative(Unique(Pattern(printPattern), "console print reference") + printPattern.Split(' ').Length);
                int command = Unique(Pattern("48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8D 6C 24 B9 48 81 EC 90 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 37 48 8B 35 ?? ?? ?? ?? 48 8B D9 48 8B CE 8B FA 48 8B 06 FF 50 08 4C 8B 0E BA"), "command system reference");
                int font = Unique(Pattern("48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? 00 00 F3 0F 10 05 ?? ?? ?? ?? F3 0F 59 ??"), "font metrics reference");
                var layout = new NativeGameLayout(_imageSize, slot, render, draw, Relative(dimensions + 5), Relative(dimensions + 44), print,
                    Relative(command + 43), Relative(font + 20) - 4, Relative(font + 20), GroundOffset());
                if(!new[] { layout.Render, layout.Draw, layout.Width, layout.Height, layout.Print }.All(IsCode) ||
                    !new[] { layout.HudSlot, layout.CommandGlobal, layout.GlyphWidth, layout.GlyphHeight }.All(InImage))
                    throw new NotSupportedException("Resolved game layout contains invalid code or data addresses");
                try { layout = layout with { Tint = ResolveTint() }; }
                catch(NotSupportedException e) { layout = layout with { TintFailure = e.Message }; }
                try {
                    layout = layout with { Cvars = new NativeCvarLayout(
                        layout.CommandGlobal + Field("engine_t", "cvarSystemForTransfer") - Field("engine_t", "cmdSystemForTransfer"),
                        Unique(Metadata("idCVar::cvarData_t").Select(m => m.Size), "cvar data size"),
                        Field("idCVar::cvarData_t", "valueString"), Field("idCVar::cvarData_t", "valueInteger"), Field("idCVar::cvarData_t", "valueFloat"),
                        Field("idCVar::cvarData_t", "name"), Field("idCVar::cvarData_t", "resetString"), Field("idCVar::cvarData_t", "flags"),
                        EnumValue("cvarFlags_t", "CVAR_CHEAT"), EnumValue("cvarFlags_t", "CVAR_NOCHEAT"),
                        EnumValue("cvarFlags_t", "CVAR_ROM"), EnumValue("cvarFlags_t", "CVAR_INIT"), EnumValue("cvarFlags_t", "CVAR_SHIPPINGDISABLED")) };
                } catch(NotSupportedException) { /* Cvar monitoring is independent of the existing HUD features. */ }
                try { layout = layout with { PlayerCheatSetters = new[] { PropertyAccessor("idPlayer", "noClip", true), PropertyAccessor("idPlayer", "noTarget", true), PropertyAccessor("idPlayer", "godMode", true) } }; }
                catch(NotSupportedException) { }
                try { layout = layout with { PlayerCheats = new(PlayerCheatFlag("noClip"), PlayerCheatFlag("noTarget"), PlayerCheatFlag("godMode")) }; }
                catch(NotSupportedException) { }
                return layout;
            }
            public void Dispose() {
                _view.SafeMemoryMappedViewHandle.ReleasePointer(); _pointer = null; _view.Dispose(); _mapping.Dispose();
            }
        }
    }
}
