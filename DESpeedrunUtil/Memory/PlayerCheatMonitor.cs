using DESpeedrunUtil.Overlay;
using System.Diagnostics;

namespace DESpeedrunUtil.Memory {
    internal readonly record struct PlayerCheatState(bool Noclip, bool Notarget, bool God) {
        internal IReadOnlyList<CvarDifference> AddTo(IReadOnlyList<CvarDifference> cvars) {
            if(!Noclip && !Notarget && !God) return cvars;
            var rows = new List<CvarDifference>(cvars.Count + 3);
            if(Noclip) rows.Add(new("noclip", "ON", "OFF", 0));
            if(Notarget) rows.Add(new("notarget", "ON", "OFF", 0));
            if(God) rows.Add(new("god", "ON", "OFF", 0));
            rows.AddRange(cvars);
            return rows;
        }
    }
    internal sealed class PlayerCheatMonitor {
        private readonly Process _game;
        private readonly NativePlayerCheatLayout _layout;
        internal PlayerCheatMonitor(Process game, NativePlayerCheatLayout layout) { _game = game; _layout = layout; }
        internal PlayerCheatState Read(IntPtr player) {
            if(player == IntPtr.Zero) return default;
            bool IsSet(NativePlayerCheatFlag flag) => _game.ReadValue<byte>(player + flag.Offset, out var value) && (value & flag.Mask) != 0;
            return new(IsSet(_layout.Noclip), IsSet(_layout.Notarget), IsSet(_layout.God));
        }
    }
}
