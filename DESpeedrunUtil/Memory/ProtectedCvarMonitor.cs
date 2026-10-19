using DESpeedrunUtil.Overlay;
using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace DESpeedrunUtil.Memory {
    internal sealed record CvarDifference(string Name, string Value, string Default, int Flags);

    // Reads the game's registry and resetString, never a guessed startup baseline.
    internal sealed class ProtectedCvarMonitor {
        private readonly Process _game;
        private readonly NativeCvarLayout _layout;
        private readonly IntPtr _image;
        private List<Entry> _entries = new();
        private int _registryCount;
        private long _lastRegistry;
        private readonly object _gate = new();
        private sealed record Entry(IntPtr Object, string Name, string Default, int Flags);
        internal ProtectedCvarMonitor(Process game, NativeCvarLayout layout) { _game = game; _layout = layout; _image = game.MainModule!.BaseAddress; }

        internal static bool IsProtected(int flags, NativeCvarLayout layout) =>
            (flags & layout.Cheat) != 0 && (flags & (layout.NoCheat | layout.ReadOnly | layout.InitOnly | layout.ShippingDisabled)) == 0;

        internal static bool IsDefault(string value, string reset, int flags) {
            static string Numeric(string text) => text.Trim().TrimEnd('f', 'F');
            if((flags & 1) != 0) {
                static bool? Boolean(string text) {
                    string normalized = text.Trim().ToLowerInvariant();
                    if(normalized == "true") return true;
                    if(normalized == "false") return false;
                    return float.TryParse(Numeric(text), NumberStyles.Float, CultureInfo.InvariantCulture, out float number) && float.IsFinite(number) ? number != 0 : null;
                }
                var a = Boolean(value); var b = Boolean(reset);
                if(a.HasValue && b.HasValue) return a == b;
            }
            if((flags & 6) != 0 && float.TryParse(Numeric(value), NumberStyles.Float, CultureInfo.InvariantCulture, out float current) &&
                float.TryParse(Numeric(reset), NumberStyles.Float, CultureInfo.InvariantCulture, out float original) && float.IsFinite(current) && float.IsFinite(original))
                return current == original;
            return string.Equals(value, reset, StringComparison.Ordinal);
        }

        private string? ReadText(IntPtr address, int limit = 512) {
            if(address.ToInt64() <= 0) return null;
            // Bounded chunks avoid reading across an unrelated unmapped page.
            var bytes = new List<byte>();
            for(int offset = 0; offset < limit; offset += 32) {
                if(!_game.ReadBytes(address + offset, 32, out var chunk)) return null;
                int end = Array.IndexOf(chunk, (byte) 0);
                bytes.AddRange(end < 0 ? chunk : chunk.Take(end));
                if(end >= 0) return Encoding.UTF8.GetString(bytes.ToArray());
            }
            return null;
        }

        internal IReadOnlyList<CvarDifference> Read() {
            lock(_gate) return ReadCore();
        }

        private IReadOnlyList<CvarDifference> ReadCore() {
            if(!_game.ReadPointer(_image + _layout.SystemGlobal, true, out var system) || system == IntPtr.Zero ||
                !_game.ReadPointer(system + 8, true, out var list) || list == IntPtr.Zero ||
                !_game.ReadBytes(list, 16, out var registry)) throw new InvalidOperationException("Cannot read cvar registry");
            var array = new IntPtr(BitConverter.ToInt64(registry, 0));
            int count = BitConverter.ToInt32(registry, 8), capacity = BitConverter.ToInt32(registry, 12);
            if(array == IntPtr.Zero || count < 1 || count > 65536 || capacity < count || capacity > 131072)
                throw new InvalidOperationException("Invalid cvar registry bounds");
            if(count != _registryCount || Environment.TickCount64 - _lastRegistry >= 5000) {
                if(!_game.ReadBytes(array, count * 8, out var pointers)) throw new InvalidOperationException("Cannot read cvar registry entries");
                var entries = new List<Entry>();
                for(int i = 0; i < count; i++) {
                    var obj = new IntPtr(BitConverter.ToInt64(pointers, i * 8));
                    if(!_game.ReadPointer(obj, true, out var data) || data == IntPtr.Zero || !_game.ReadBytes(data, _layout.DataSize, out var bytes)) continue;
                    int flags = BitConverter.ToInt32(bytes, _layout.Flags);
                    if(!IsProtected(flags, _layout)) continue;
                    string? name = ReadText(new IntPtr(BitConverter.ToInt64(bytes, _layout.Name)), 256);
                    string? reset = ReadText(new IntPtr(BitConverter.ToInt64(bytes, _layout.Reset)));
                    if(string.IsNullOrEmpty(name) || reset == null || !name.All(c => c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '_')) continue;
                    entries.Add(new Entry(obj, name, reset, flags));
                }
                _entries = entries; _registryCount = count; _lastRegistry = Environment.TickCount64;
            }
            var changed = new List<CvarDifference>();
            foreach(var entry in _entries) {
                if(!_game.ReadPointer(entry.Object, true, out var data) || data == IntPtr.Zero || !_game.ReadBytes(data, _layout.DataSize, out var bytes)) continue;
                string? value = ReadText(new IntPtr(BitConverter.ToInt64(bytes, _layout.ValueString)));
                if(value != null && !IsDefault(value, entry.Default, entry.Flags)) changed.Add(new(entry.Name, value, entry.Default, entry.Flags));
            }
            return changed.OrderBy(c => c.Name, StringComparer.OrdinalIgnoreCase).ToArray();
        }
    }
}
