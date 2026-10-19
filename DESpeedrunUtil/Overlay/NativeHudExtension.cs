using DESpeedrunUtil.Memory;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace DESpeedrunUtil.Overlay {
    internal sealed class NativeHudExtension : IDisposable {
        internal const int PointerOffset = 1016, RecordSize = 256, MaxLines = 512, BankSize = 16 + MaxLines * RecordSize;
        private const int StateSize = 4096 + BankSize * 2;
        private readonly Process _game;
        private readonly IntPtr _state;
        private readonly NativeGameLayout _layout;
        private readonly SemaphoreSlim _actions = new(1, 1);
        private int _bank;
        private IReadOnlyList<CvarDifference>? _published;
        private Size _publishedViewport;
        private double _publishedAnchor;
        private float _publishedScale;
        private bool _publishedBelow;
        private float? _publishedRight;
        private float _publishedGlyphWidth, _publishedGlyphHeight;
        internal IntPtr State => _state;
        internal SizeF VirtualViewport {
            get {
                float width = _game.ReadValue<float>(_state + 112, 1920), height = _game.ReadValue<float>(_state + 116, 1080);
                return new(float.IsFinite(width) && width > 0 ? width : 1920, float.IsFinite(height) && height > 0 ? height : 1080);
            }
        }
        internal NativeHudExtension(Process game, IntPtr owner, NativeGameLayout layout) {
            _game = game; _layout = layout;
            if(game.ReadPointer(owner + PointerOffset, true, out var previous) && previous != IntPtr.Zero &&
                game.ReadString(previous + 16, ReadStringType.ASCII, 8) == "DESRUE01") {
                _state = previous; _bank = game.ReadPointer(_state + 8) == _state + 4096 ? 0 : 1; return;
            }
            IntPtr allocation = game.AllocateMemory(4096 + StateSize);
            if(allocation == IntPtr.Zero) throw new InvalidOperationException("Cannot allocate cvar HUD");
            _state = allocation + 4096;
            try {
                var bytes = new byte[4096];
                BitConverter.GetBytes(allocation.ToInt64()).CopyTo(bytes, 0);
                Encoding.ASCII.GetBytes("DESRUE01").CopyTo(bytes, 16);
                for(int i = 0; i < 4; i++) BitConverter.GetBytes(1f).CopyTo(bytes, 64 + 4 * i);
                BitConverter.GetBytes(1920f).CopyTo(bytes, 80); BitConverter.GetBytes(1080f).CopyTo(bytes, 84);
                BitConverter.GetBytes(1920f).CopyTo(bytes, 112); BitConverter.GetBytes(1080f).CopyTo(bytes, 116);
                IntPtr image = game.MainModule!.BaseAddress;
                byte[] code = CreateCode(image + layout.Draw, image + layout.Width, image + layout.Height);
                if(!game.WriteBytes(_state, bytes) || !game.WriteBytes(allocation, code) ||
                    !game.VirtualProtect(allocation, 4096, MemPageProtect.PAGE_EXECUTE_READ) ||
                    !game.VirtualProtect(_state, StateSize, MemPageProtect.PAGE_READWRITE) ||
                    !FlushInstructionCache(game.Handle, allocation, (UIntPtr) code.Length) ||
                    !game.WriteBytes(owner + PointerOffset, BitConverter.GetBytes(_state.ToInt64())))
                    throw new InvalidOperationException("Cannot attach cvar HUD");
            } catch { game.FreeMemory(allocation); throw; }
        }

        internal static byte[] CreateCode(IntPtr draw, IntPtr width, IntPtr height) {
            var c = new NativeSpeedometerCode.Code();
            c.Emit("535657 4883EC40 488BD9 488BF2"); // state, GUI; aligned calls
            c.Emit("837B2001"); c.Jump("0F85", "actionsDone");
            c.Emit("C7432002000000 488B4B30 488B5338 488B4328 FFD0 48894378 894324 C7432003000000");
            c.Label("actionsDone");
            c.Emit("837B1800"); c.Jump("0F8E", "done");
            c.Emit("F0FF4B18 488BCE"); c.Call(width.ToInt64()); c.Emit("F30F114370");
            c.Emit("488BCE"); c.Call(height.ToInt64()); c.Emit("F30F114374");
            c.Emit("488B7B08 4885FF"); c.Jump("0F84", "done");
            c.Emit("8B4700 89442438 4883C710");
            c.Label("line");
            c.Emit("837C243800"); c.Jump("0F8E", "done");
            // X is a physical right edge; record +12 stores measured virtual text width.
            c.Emit("F30F100F F30F594B70 F30F5E4B50 F30F5C4F0C 660FEFC0 F30F5FC8");
            c.Emit("F30F105704 F30F595374 F30F5E5354 660FEFC0 F30F5FD0");
            c.Emit("488BCE 4C8D4F10 488D4340 4889442420 C744242801000000 F30F104708 F30F11442430");
            c.Call(draw.ToInt64());
            c.Emit("4881C700010000 FF4C2438"); c.Jump("E9", "line");
            c.Label("done"); c.Emit("4883C440 5F5E5B C3");
            return c.Finish();
        }

        internal void Update(IReadOnlyList<CvarDifference> differences, Rectangle viewport, double anchorY, float scale, float glyphWidth, float glyphHeight, bool visible, bool belowAnchor = false, float? rightEdge = null) {
            if(!visible || differences.Count == 0) { _game.WriteBytes(_state + 24, new byte[4]); return; }
            if(ReferenceEquals(_published, differences) && _publishedViewport == viewport.Size && _publishedAnchor == anchorY && _publishedScale == scale &&
                _publishedBelow == belowAnchor && _publishedRight == rightEdge && _publishedGlyphWidth == glyphWidth && _publishedGlyphHeight == glyphHeight) {
                _game.WriteBytes(_state + 24, BitConverter.GetBytes(240)); return;
            }
            float virtualHeight = _game.ReadValue<float>(_state + 116, 1080);
            float virtualWidth = _game.ReadValue<float>(_state + 112, 1920);
            if(!float.IsFinite(virtualHeight) || virtualHeight <= 0 || !float.IsFinite(virtualWidth) || virtualWidth <= 0) return;
            float physicalLine = glyphHeight * scale * 1.15f * viewport.Height / virtualHeight;
            float origin = (float) Math.Clamp(belowAnchor ? anchorY : anchorY - physicalLine, physicalLine, viewport.Height - physicalLine);
            int rows = Math.Max(1, (int) ((belowAnchor ? viewport.Height - origin - physicalLine - 12 : origin - 12) / physicalLine) + 1);
            var texts = differences.Select(d => d.Name + " = " + d.Value.Replace('\n', ' ').Replace('\r', ' ')).ToArray();
            int columns = (texts.Length + rows - 1) / rows;
            // Keep all values on screen; shrink only further if multiple columns require it.
            float columnWidth = texts.Max(t => t.Length) * glyphWidth * scale * viewport.Width / virtualWidth + 20;
            if(columns * columnWidth > viewport.Width - 24 && scale > 0.15f) {
                Update(differences, viewport, anchorY, Math.Max(0.15f, scale * 0.8f), glyphWidth, glyphHeight, visible, belowAnchor, rightEdge); return;
            }
            int count = Math.Min(MaxLines, texts.Length);
            var bytes = new byte[16 + count * RecordSize];
            BitConverter.GetBytes(count).CopyTo(bytes, 0);
            for(int i = 0; i < count; i++) {
                int at = 16 + i * RecordSize;
                string text = texts[i];
                if(text.Length > 238) text = text[..235] + "...";
                BitConverter.GetBytes((rightEdge ?? viewport.Width - 12f) - (i / rows) * columnWidth).CopyTo(bytes, at);
                BitConverter.GetBytes(origin + (belowAnchor ? 1 : -1) * (i % rows) * physicalLine).CopyTo(bytes, at + 4);
                BitConverter.GetBytes(scale).CopyTo(bytes, at + 8);
                BitConverter.GetBytes(text.Length * glyphWidth * scale).CopyTo(bytes, at + 12);
                Encoding.ASCII.GetBytes(text).CopyTo(bytes, at + 16);
            }
            _bank ^= 1;
            IntPtr bank = _state + 4096 + _bank * BankSize;
            if(!_game.WriteBytes(bank, bytes) || !_game.WriteBytes(_state + 80, BitConverter.GetBytes((float) viewport.Width).Concat(BitConverter.GetBytes((float) viewport.Height)).ToArray()) ||
                !_game.WriteBytes(_state + 8, BitConverter.GetBytes(bank.ToInt64())) || !_game.WriteBytes(_state + 24, BitConverter.GetBytes(240)))
                throw new InvalidOperationException("Cannot update protected cvar display");
            _published = differences; _publishedViewport = viewport.Size; _publishedAnchor = anchorY; _publishedScale = scale;
            _publishedBelow = belowAnchor; _publishedRight = rightEdge; _publishedGlyphWidth = glyphWidth; _publishedGlyphHeight = glyphHeight;
        }

        internal async Task<int> InvokeAsync(IntPtr function, IntPtr first = default, IntPtr second = default, int timeoutMs = 20000) {
            if(function == IntPtr.Zero) throw new ArgumentException("Missing game action");
            await _actions.WaitAsync();
            try {
                if(_game.ReadValue<int>(_state + 32) is 1 or 2) throw new InvalidOperationException("A previous game action is still running");
                byte[] request = BitConverter.GetBytes(function.ToInt64()).Concat(BitConverter.GetBytes(first.ToInt64())).Concat(BitConverter.GetBytes(second.ToInt64())).ToArray();
                if(!_game.WriteBytes(_state + 40, request) || !_game.WriteBytes(_state + 32, BitConverter.GetBytes(1))) throw new InvalidOperationException("Cannot publish game action");
                var timer = Stopwatch.StartNew();
                while(timer.ElapsedMilliseconds < timeoutMs) {
                    if(_game.HasExited) throw new InvalidOperationException("The game closed during the action");
                    if(_game.ReadValue<int>(_state + 32) == 3) return _game.ReadValue<int>(_state + 36);
                    await Task.Delay(25);
                }
                // Never free memory or claim failure cancelled a function that may still be executing.
                throw new TimeoutException("The game has not acknowledged the action yet");
            } finally { _actions.Release(); }
        }

        internal async Task ExecuteAsync(string commands) {
            IntPtr image = _game.MainModule!.BaseAddress;
            IntPtr system = _game.ReadPointer(image + _layout.CommandGlobal), vtable = _game.ReadPointer(system), execute = _game.ReadPointer(vtable + 64);
            byte[] text = Encoding.UTF8.GetBytes(commands + '\0');
            IntPtr buffer = _game.AllocateMemory(text.Length);
            if(buffer == IntPtr.Zero || !_game.WriteBytes(buffer, text)) throw new InvalidOperationException("Cannot allocate console action");
            bool acknowledged = false;
            try { await InvokeAsync(execute, system, buffer); acknowledged = true; }
            finally { if(acknowledged) _game.FreeMemory(buffer); }
        }

        internal async Task SetCvarAsync(string name, string value) {
            // Call idCVarSystem::SetCVarString on the HUD thread. Passing strings
            // directly preserves quotes, semicolons and paths without console parsing.
            if(_layout.Cvars == null) throw new NotSupportedException("The native cvar setter is unavailable");
            IntPtr system = _game.ReadPointer(_game.MainModule!.BaseAddress + _layout.Cvars.SystemGlobal);
            IntPtr setter = _game.ReadPointer(_game.ReadPointer(system) + 56);
            if(system == IntPtr.Zero || setter == IntPtr.Zero) throw new NotSupportedException("The native cvar setter is unavailable");
            byte[] key = Encoding.UTF8.GetBytes(name + '\0'), text = Encoding.UTF8.GetBytes(value + '\0');
            IntPtr allocation = _game.AllocateMemory(4096 + 32 + key.Length + text.Length);
            if(allocation == IntPtr.Zero) throw new InvalidOperationException("Cannot allocate cvar restore action");
            IntPtr arguments = allocation + 4096;
            byte[] data = new byte[32 + key.Length + text.Length];
            BitConverter.GetBytes(system.ToInt64()).CopyTo(data, 0);
            BitConverter.GetBytes((arguments + 32).ToInt64()).CopyTo(data, 8);
            BitConverter.GetBytes((arguments + 32 + key.Length).ToInt64()).CopyTo(data, 16);
            BitConverter.GetBytes(setter.ToInt64()).CopyTo(data, 24);
            key.CopyTo(data, 32); text.CopyTo(data, 32 + key.Length);
            byte[] code = Convert.FromHexString("4883EC28 488B4118 4C8B4110 488B5108 488B09 4533C9 FFD0 4883C428 C3".Replace(" ", ""));
            bool acknowledged = false;
            try {
                if(!_game.WriteBytes(arguments, data) || !_game.WriteBytes(allocation, code) ||
                    !_game.VirtualProtect(allocation, 4096, MemPageProtect.PAGE_EXECUTE_READ) ||
                    !FlushInstructionCache(_game.Handle, allocation, (UIntPtr) code.Length)) throw new InvalidOperationException("Cannot publish cvar restore action");
                await InvokeAsync(allocation, arguments); acknowledged = true;
            } finally { if(acknowledged) _game.FreeMemory(allocation); }
        }

        public void Dispose() {
            try { if(!_game.HasExited) _game.WriteBytes(_state + 24, new byte[4]); }
            catch(InvalidOperationException) { }
            catch(System.ComponentModel.Win32Exception) { }
        }
        [DllImport("kernel32.dll")] private static extern bool FlushInstructionCache(IntPtr process, IntPtr address, UIntPtr length);
    }
}
