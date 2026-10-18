using DESpeedrunUtil.Memory;
using System.Diagnostics;

namespace DESpeedrunUtil.Overlay {
    internal sealed class LowHealthTintTarget {
        private readonly Process _game;
        private readonly NativeTintLayout? _layout;
        private long _nextRefresh;
        private (long Handle, int Low, int VeryLow)? _published;
        private IntPtr _lastPlayer;
        private long _lastHandle = -1;
        internal LowHealthTintTarget(Process game) {
            _game = game;
            _layout = NativeGameLayout.Resolve(game.MainModule!.FileName).Tint;
        }

        internal void Update(IntPtr state, IntPtr player = default) {
            long now = Environment.TickCount64;
            if(now < _nextRefresh) return;
            _nextRefresh = now + 100;
            var target = Resolve(player);
            if(_published == target) return;
            // Resolve first; do not briefly expose an unfiltered frame on every refresh.
            if(!_game.WriteBytes(state + 232, BitConverter.GetBytes(-1L))) return;
            byte[] indices = new byte[8];
            BitConverter.GetBytes(target.Low).CopyTo(indices, 0); BitConverter.GetBytes(target.VeryLow).CopyTo(indices, 4);
            if(_game.WriteBytes(state + 240, indices) && _game.WriteBytes(state + 232, BitConverter.GetBytes(target.Handle))) _published = target;
        }

        private (long Handle, int Low, int VeryLow) Resolve(IntPtr player) {
            var disabled = (-1L, -1, -1);
            if(_layout == null) return disabled;
            var layout = _layout;
            IntPtr image = _game.MainModule!.BaseAddress;
            if(player == IntPtr.Zero && _game.MainModule.ModuleMemorySize == NativeSpeedometer.ModuleSize11) {
                if(!_game.ReadPointer(image + 80143112, true, out var root) || root == IntPtr.Zero ||
                !_game.ReadPointer(root + 0x1510, true, out var slot) || slot == IntPtr.Zero ||
                !_game.ReadPointer(slot + 0x598, true, out player)) return disabled;
            }
            if(player == IntPtr.Zero || !_game.ReadPointer(player, true, out var vtable) || vtable != image + layout.PlayerVtable ||
                !_game.ReadPointer(player + layout.ScreenDeclOffset, true, out var decl) || decl == IntPtr.Zero ||
                !_game.ReadPointer(decl, true, out var declVtable) || declVtable != image + layout.DeclVtable ||
                !_game.ReadPointer(decl + layout.ActionsOffset, true, out var actions) || actions == IntPtr.Zero ||
                !_game.ReadValue<int>(decl + layout.ActionsOffset + 8, out var count) || count < 1 || count > 256 ||
                !_game.ReadValue<int>(player + layout.HealthSlotOffset, out var fxSlot)) return disabled;
            long handle = -1;
            if(fxSlot >= 0 && fxSlot <= 3) {
                if(!_game.ReadValue<long>(player + layout.ScreenGroupsOffset + fxSlot * 32, out handle)) return disabled;
            } else if(fxSlot == -1 && player == _lastPlayer && _lastHandle != -1) {
                // The health condition can end before its visual fade-out finishes. Retain
                // ownership only while that same group still belongs to the same player.
                for(int i = 0; i < 4; i++)
                    if(_game.ReadValue<long>(player + layout.ScreenGroupsOffset + i * 32, out var candidate) && candidate == _lastHandle) handle = candidate;
            }
            if(handle == -1) return disabled;
            int low = -1, veryLow = -1;
            for(int i = 0; i < count; i++) {
                IntPtr action = actions + i * layout.ActionSize;
                if(!_game.ReadValue<int>(action + layout.ActionTypeOffset, out var type) || type != layout.VignetteActionType ||
                    !_game.ReadValue<int>(action + layout.ConditionsOffset + 8, out var conditionCount) || conditionCount != 1 ||
                    !_game.ReadPointer(action + layout.ConditionsOffset, true, out var conditions) || conditions == IntPtr.Zero ||
                    !_game.ReadValue<int>(conditions, out var condition)) continue;
                if(condition == layout.LowCondition) low = i;
                if(condition == layout.VeryLowCondition) veryLow = i;
            }
            if(low < 0 || veryLow < 0) return disabled;
            _lastPlayer = player; _lastHandle = handle;
            return (handle, low, veryLow);
        }
    }
}
