using DESpeedrunUtil.Overlay;
using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace DESpeedrunUtil.Memory {
    internal sealed record CvarRestore(string Name, string? Value);

    internal sealed class CheatCvarSession {
        private readonly Process _game;
        private readonly NativeHudExtension _hud;
        private readonly ProtectedCvarMonitor _monitor;
        private IReadOnlyList<CvarDifference> _baseline = Array.Empty<CvarDifference>();
        private IntPtr _storage;
        private readonly SemaphoreSlim _begin = new(1, 1);
        private IReadOnlyList<CvarDifference>? _displaySource;
        private IReadOnlyList<CvarDifference> _displayValues = Array.Empty<CvarDifference>();
        internal bool Active { get; private set; }
        internal CheatCvarSession(Process game, NativeHudExtension hud, ProtectedCvarMonitor monitor) {
            _game = game; _hud = hud; _monitor = monitor;
            _storage = game.ReadPointer(hud.State + 144);
            if(_storage == IntPtr.Zero) return;
            byte[] header = game.ReadBytes(_storage, 16) ?? throw new InvalidOperationException("Cannot read cheat cvar session");
            if(Encoding.ASCII.GetString(header, 0, 8) != "DESRUB01") throw new NotSupportedException("Unknown cheat cvar session");
            Active = BitConverter.ToInt32(header, 8) == 1;
            int size = BitConverter.ToInt32(header, 12);
            if(size < 2 || size > 16 * 1024 * 1024) throw new InvalidOperationException("Invalid cheat cvar session size");
            _baseline = JsonSerializer.Deserialize<CvarDifference[]>(game.ReadBytes(_storage + 16, size) ?? throw new InvalidOperationException("Cannot read initial cvar values"))
                ?? throw new InvalidOperationException("Invalid initial cvar values");
        }

        internal async Task BeginAsync() {
            await _begin.WaitAsync();
            try {
                if(Active) return;
                var baseline = await Task.Run(_monitor.Read);
                byte[] text = JsonSerializer.SerializeToUtf8Bytes(baseline);
                var data = new byte[16 + text.Length];
                Encoding.ASCII.GetBytes("DESRUB01").CopyTo(data, 0);
                BitConverter.GetBytes(1).CopyTo(data, 8); BitConverter.GetBytes(text.Length).CopyTo(data, 12); text.CopyTo(data, 16);
                IntPtr storage = _game.AllocateMemory(data.Length);
                if(storage == IntPtr.Zero || !_game.WriteBytes(storage, data) || !_game.VirtualProtect(storage, data.Length, MemPageProtect.PAGE_READWRITE) ||
                    !_game.WriteBytes(_hud.State + 144, BitConverter.GetBytes(storage.ToInt64()))) throw new InvalidOperationException("Cannot preserve initial cvar values");
                _storage = storage; _baseline = baseline; Active = true;
                _displaySource = null;
            } finally { _begin.Release(); }
        }

        internal static IReadOnlyList<CvarRestore> Restores(IReadOnlyList<CvarDifference> baseline, IReadOnlyList<CvarDifference> current) {
            var before = baseline.ToDictionary(c => c.Name, StringComparer.OrdinalIgnoreCase);
            var now = current.ToDictionary(c => c.Name, StringComparer.OrdinalIgnoreCase);
            var result = new List<CvarRestore>();
            foreach(var item in current) {
                if(!before.TryGetValue(item.Name, out var old)) result.Add(new(item.Name, null)); // initially at factory default
                else if(!ProtectedCvarMonitor.IsDefault(item.Value, old.Value, old.Flags)) result.Add(new(item.Name, old.Value));
            }
            foreach(var old in baseline)
                if(!now.ContainsKey(old.Name)) result.Add(new(old.Name, old.Value)); // changed from a user value to factory default
            return result;
        }

        internal static IReadOnlyList<CvarDifference> DisplayChanges(IReadOnlyList<CvarDifference> baseline, IReadOnlyList<CvarDifference> current) {
            // Initial nondefault values are preserved preferences, not cheat-session
            // changes. Keep these names out of the HUD even if changed again later.
            var preserved = new HashSet<string>(baseline.Select(c => c.Name), StringComparer.OrdinalIgnoreCase);
            return current.Where(c => !preserved.Contains(c.Name) && !ProtectedCvarMonitor.IsDefault(c.Value, c.Default, c.Flags)).ToArray();
        }

        internal IReadOnlyList<CvarDifference> DisplayChanges(IReadOnlyList<CvarDifference> current) {
            if(!Active) return Array.Empty<CvarDifference>();
            if(!ReferenceEquals(current, _displaySource)) {
                _displayValues = DisplayChanges(_baseline, current);
                _displaySource = current;
            }
            return _displayValues;
        }

        internal async Task<int> RestoreAsync() {
            if(!Active) throw new InvalidOperationException("No initial cvar snapshot exists for this cheat session");
            var restores = Restores(_baseline, await Task.Run(_monitor.Read));
            foreach(var restore in restores) {
                if(restore.Value == null) await _hud.ExecuteAsync("reset " + restore.Name);
                else await _hud.SetCvarAsync(restore.Name, restore.Value);
            }
            if(Restores(_baseline, await Task.Run(_monitor.Read)).Count != 0)
                throw new InvalidOperationException("The game did not restore all protected cvars; cheats were left enabled so restoration can be retried");
            return restores.Count;
        }

        internal void End() {
            if(_storage != IntPtr.Zero && !_game.WriteBytes(_storage + 8, new byte[4])) throw new InvalidOperationException("Cannot finish cheat cvar session");
            Active = false;
        }
    }
}
