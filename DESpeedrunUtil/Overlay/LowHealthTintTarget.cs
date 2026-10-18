using DESpeedrunUtil.Memory;
using System.Diagnostics;

namespace DESpeedrunUtil.Overlay {
    internal sealed class LowHealthTintTarget {
        private readonly Process _game;
        private long _nextRefresh;
        private (long Handle, int Low, int VeryLow)? _published;
        private IntPtr _lastPlayer;
        private long _lastHandle = -1;
        internal LowHealthTintTarget(Process game) => _game = game;

        internal void Update(IntPtr state) {
            long now = Environment.TickCount64;
            if(now < _nextRefresh) return;
            _nextRefresh = now + 100;
            var target = Resolve();
            if(_published == target) return;
            // Resolve first; do not briefly expose an unfiltered frame on every refresh.
            if(!_game.WriteBytes(state + 232, BitConverter.GetBytes(-1L))) return;
            byte[] indices = new byte[8];
            BitConverter.GetBytes(target.Low).CopyTo(indices, 0); BitConverter.GetBytes(target.VeryLow).CopyTo(indices, 4);
            if(_game.WriteBytes(state + 240, indices) && _game.WriteBytes(state + 232, BitConverter.GetBytes(target.Handle))) _published = target;
        }

        private (long Handle, int Low, int VeryLow) Resolve() {
            var disabled = (-1L, -1, -1);
            IntPtr image = _game.MainModule!.BaseAddress;
            if(!_game.ReadPointer(image + 80143112, true, out var root) || root == IntPtr.Zero ||
                !_game.ReadPointer(root + 0x1510, true, out var slot) || slot == IntPtr.Zero ||
                !_game.ReadPointer(slot + 0x598, true, out var player) || player == IntPtr.Zero ||
                !_game.ReadPointer(player, true, out var vtable) || vtable != image + 0x2984578 ||
                !_game.ReadPointer(player + 0x37C78, true, out var decl) || decl == IntPtr.Zero ||
                !_game.ReadPointer(decl, true, out var declVtable) || declVtable != image + 0x262A7B8 ||
                !_game.ReadPointer(decl + 0xA0, true, out var actions) || actions == IntPtr.Zero ||
                !_game.ReadValue<int>(decl + 0xA8, out var count) || count < 1 || count > 256 ||
                !_game.ReadValue<int>(player + 0x25F60, out var fxSlot)) return disabled;
            long handle = -1;
            if(fxSlot >= 0 && fxSlot <= 3) {
                if(!_game.ReadValue<long>(player + 0x37C80 + fxSlot * 32, out handle)) return disabled;
            } else if(fxSlot == -1 && player == _lastPlayer && _lastHandle != -1) {
                // The health condition can end before its visual fade-out finishes. Retain
                // ownership only while that same group still belongs to the same player.
                for(int i = 0; i < 4; i++)
                    if(_game.ReadValue<long>(player + 0x37C80 + i * 32, out var candidate) && candidate == _lastHandle) handle = candidate;
            }
            if(handle == -1) return disabled;
            int low = -1, veryLow = -1;
            for(int i = 0; i < count; i++) {
                IntPtr action = actions + i * 0x6C0;
                if(!_game.ReadValue<int>(action + 0x14, out var type) || type != 23 ||
                    !_game.ReadValue<int>(action + 0x98, out var conditionCount) || conditionCount != 1 ||
                    !_game.ReadPointer(action + 0x90, true, out var conditions) || conditions == IntPtr.Zero ||
                    !_game.ReadValue<int>(conditions, out var condition)) continue;
                if(condition == 499) low = i;
                if(condition == 500) veryLow = i;
            }
            if(low < 0 || veryLow < 0) return disabled;
            _lastPlayer = player; _lastHandle = handle;
            return (handle, low, veryLow);
        }
    }
}
