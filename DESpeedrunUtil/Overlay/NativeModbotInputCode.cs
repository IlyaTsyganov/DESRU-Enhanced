namespace DESpeedrunUtil.Overlay {
    // Hooks never alter game input state, bindings, cvars or widget enable flags.
    // Only a modbot-owned tab-navigation event is consumed during the opening hold.
    internal static class NativeModbotInputCode {
        internal const string Magic = "DESRUMI1";
        internal const int Enabled = 8, Lease = 16, Owner = 24, Tabs = 32, Root = 40,
            Held = 48, Phase = 52, Shows = 56, Suppressed = 60;

        internal static byte[] Create(int kind, long state, long original, long keys, long clock, NativeModbotInputLayout layout) {
            var c = new NativeSpeedometerCode.Code();
            void Offset(int offset) => c.Bytes.AddRange(BitConverter.GetBytes(offset));
            void Clear() => c.Emit("48C7431800000000 48C7432000000000 48C7432800000000 C7433000000000 C7433400000000");
            void Live(string skip) {
                c.Emit("837B0800");c.Jump("0F84",skip);
                c.Call(clock);c.Emit("483B4310");c.Jump("0F83",skip);
            }
            void Poll(string suffix) {
                c.Emit("F7433001000000");c.Jump("0F84","q"+suffix);
                c.Emit("B945000000");c.Call(keys);c.Emit("66A90080");c.Jump("0F85","q"+suffix);
                c.Emit("836330FE");c.Label("q"+suffix);
                c.Emit("F7433002000000");c.Jump("0F84","polled"+suffix);
                c.Emit("B951000000");c.Call(keys);c.Emit("66A90080");c.Jump("0F85","polled"+suffix);
                c.Emit("836330FD");c.Label("polled"+suffix);
            }
            // 3 nonvolatile saves + 32-byte shadow space: calls are 16-byte aligned.
            c.Emit("535657 4883EC20 488BF1 488BFA 48BB");c.Pointer(state);
            if(kind == 0) { // OnShow: capture the opening hold BEFORE the menu runs.
                Clear();Live("original");
                c.Emit("48897318 488B86");Offset(layout.TabsOffset);c.Emit("48894320 488B86");Offset(layout.RootOffset);c.Emit("48894328");
                c.Emit("C7433003000000");Poll("show");
                c.Emit("837B3000");c.Jump("0F84","original");c.Emit("C7433401000000 FF4338");
            } else if(kind == 1) { // Think: observe physical release, then drain one UI update.
                c.Emit("488BCE 488BD7");c.Call(original);
                c.Emit("48397318");c.Jump("0F85","done");
                Live("clear");c.Emit("837B3402");c.Jump("0F84","clear");
                c.Emit("837B3401");c.Jump("0F85","done");Poll("think");
                c.Emit("837B3000");c.Jump("0F85","done");c.Emit("C7433402000000");c.Jump("E9","done");
                c.Label("clear");Clear();c.Jump("E9","done");
            } else if(kind == 2) { // ReceiveEvent(this,event): pass every other widget/event through.
                c.Emit("48397320");c.Jump("0F85","original");
                c.Emit("488B86");Offset(layout.ParentOffset);c.Emit("483B4328");c.Jump("0F85","original");
                c.Emit("4885FF");c.Jump("0F84","original");
                c.Emit("8B87");Offset(layout.EventTypeOffset);c.Emit("83E8");c.Bytes.Add(checked((byte)layout.NavigationEventFirst));c.Emit("83F801");c.Jump("0F87","original");
                c.Emit("837B3400");c.Jump("0F84","original");Live("original");
                c.Emit("FF433C B801000000");c.Jump("E9","done");
            } else if(kind == 3) { // OnHide: never carry a latch into another menu/session.
                c.Emit("48397318");c.Jump("0F85","original");Clear();
            } else throw new ArgumentOutOfRangeException(nameof(kind));
            if(kind != 1) { c.Label("original");c.Emit("488BCE 488BD7");c.Call(original); }
            c.Label("done");c.Emit("4883C420 5F5E5B C3");
            byte[] result=c.Finish();
            if(result.Length>1024) throw new InvalidOperationException("Modbot guard exceeds its code slot");
            return result;
        }
    }
}
