using DESpeedrunUtil.Memory;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace DESpeedrunUtil.Overlay {
    internal sealed class NativeModbotInputGuard : IDisposable {
        private const int PointerOffset=152;
        private readonly Process _game;
        private readonly IntPtr _state;
        internal IntPtr State => _state;
        internal int SuppressedEvents => _game.ReadValue<int>(_state+NativeModbotInputCode.Suppressed);
        private NativeModbotInputGuard(Process game,IntPtr state) { _game=game;_state=state; }

        internal static NativeModbotInputGuard Attach(Process game,NativeHudExtension hud,NativeGameLayout native) {
            var layout=native.ModbotInput??throw new NotSupportedException(native.ModbotInputFailure??"Modbot input layout unavailable");
            IntPtr image=game.MainModule!.BaseAddress;
            IntPtr[] slots={image+layout.MenuVtable+layout.ShowSlot*8,image+layout.MenuVtable+layout.ThinkSlot*8,image+layout.TabsVtable+layout.ReceiveSlot*8,image+layout.MenuVtable+layout.HideSlot*8};
            IntPtr[] originals={image+layout.Show,image+layout.Think,image+layout.Receive,image+layout.Hide};
            IntPtr keys=game.ReadPointer(image+layout.AsyncKeyImport),clock=RemoteApi(game,"kernel32.dll","GetTickCount64");
            if(keys==IntPtr.Zero||clock==IntPtr.Zero)throw new NativeGameNotReadyException("Input API imports are not ready");
            byte[][] Code(IntPtr state)=>Enumerable.Range(0,4).Select(i=>NativeModbotInputCode.Create(i,state.ToInt64(),originals[i].ToInt64(),keys.ToInt64(),clock.ToInt64(),layout)).ToArray();
            IntPtr previous=game.ReadPointer(hud.State+PointerOffset);
            if(previous!=IntPtr.Zero) {
                byte[] marker=game.ReadBytes(previous,8)??Array.Empty<byte>();
                if(Encoding.ASCII.GetString(marker)!=NativeModbotInputCode.Magic)throw new NotSupportedException("Unknown modbot input guard state");
                IntPtr allocation=previous-4096;byte[][] expected=Code(previous);
                for(int i=0;i<4;i++) if(game.ReadPointer(slots[i])!=allocation+i*1024 ||
                    !(game.ReadBytes(allocation+i*1024,expected[i].Length)??Array.Empty<byte>()).SequenceEqual(expected[i]))
                    throw new NotSupportedException("Modbot input guard no longer owns its callbacks");
                return new(game,previous);
            }
            for(int i=0;i<4;i++) if(game.ReadPointer(slots[i])!=originals[i])
                throw new NotSupportedException("Modbot input handler was changed by another module");
            IntPtr memory=game.AllocateMemory(8192);
            if(memory==IntPtr.Zero)throw new InvalidOperationException("Cannot allocate modbot input guard");
            IntPtr state=memory+4096;bool referenced=false;int installed=0;
            try {
                var page=new byte[4096];Encoding.ASCII.GetBytes(NativeModbotInputCode.Magic).CopyTo(page,0);
                if(!game.WriteBytes(state,page))throw new InvalidOperationException("Cannot initialize modbot input state");
                byte[][] code=Code(state);
                for(int i=0;i<4;i++)if(!game.WriteBytes(memory+i*1024,code[i]))throw new InvalidOperationException("Cannot initialize modbot input callback");
                if(!game.VirtualProtect(memory,4096,MemPageProtect.PAGE_EXECUTE_READ)||
                    !game.VirtualProtect(state,4096,MemPageProtect.PAGE_READWRITE)||
                    !FlushInstructionCache(game.Handle,memory,(UIntPtr)4096))throw new InvalidOperationException("Cannot protect modbot input callbacks");
                // Only aligned function pointers change. Code/data are complete before publishing.
                // No game instructions are patched or game objects/input queues modified.
                for(int i=0;i<4;i++) {
                    if(game.ReadPointer(slots[i])!=originals[i])throw new InvalidOperationException("Modbot handler changed while attaching");
                    if(!game.VirtualProtect(slots[i],8,MemPageProtect.PAGE_READWRITE,out var protection))throw new InvalidOperationException("Cannot protect modbot virtual slot");
                    try {
                        referenced=true;
                        if(!game.WriteBytes(slots[i],BitConverter.GetBytes((memory+i*1024).ToInt64())))throw new InvalidOperationException("Cannot publish modbot input callback");
                        installed++;
                    } finally { game.VirtualProtect(slots[i],8,protection); }
                }
                if(!game.WriteBytes(hud.State+PointerOffset,BitConverter.GetBytes(state.ToInt64())))throw new InvalidOperationException("Cannot preserve modbot input guard");
                return new(game,state);
            } catch {
                // Disabled callbacks pass through. Keep their code alive if a thread may have fetched it.
                game.WriteBytes(state+NativeModbotInputCode.Enabled,new byte[4]);
                for(int i=0;i<installed;i++) {
                    if(game.ReadPointer(slots[i])!=memory+i*1024)continue;
                    if(game.VirtualProtect(slots[i],8,MemPageProtect.PAGE_READWRITE,out var protection)) {
                        try { game.WriteBytes(slots[i],BitConverter.GetBytes(originals[i].ToInt64())); }
                        finally { game.VirtualProtect(slots[i],8,protection); }
                    }
                }
                throw;
            } finally { if(!referenced)game.FreeMemory(memory); }
        }
        internal void Update(bool enabled) {
            if(!_game.WriteBytes(_state+NativeModbotInputCode.Lease,BitConverter.GetBytes(Environment.TickCount64+3000))||
                !_game.WriteBytes(_state+NativeModbotInputCode.Enabled,BitConverter.GetBytes(enabled?1:0)))
                throw new InvalidOperationException("Cannot renew modbot input guard");
        }
        public void Dispose() {
            try { if(!_game.HasExited)_game.WriteBytes(_state+NativeModbotInputCode.Enabled,new byte[4]); }
            catch(InvalidOperationException) { }
            catch(System.ComponentModel.Win32Exception) { }
        }
        private static IntPtr RemoteApi(Process game,string module,string name) {
            IntPtr address=GetProcAddress(GetModuleHandle(module),name);
            if(address==IntPtr.Zero)throw new NotSupportedException("Missing Windows input guard API");
            using var current=Process.GetCurrentProcess();
            var owner=current.Modules.Cast<ProcessModule>().Single(m=>address.ToInt64()>=m.BaseAddress.ToInt64()&&address.ToInt64()<m.BaseAddress.ToInt64()+m.ModuleMemorySize);
            var remote=game.Modules.Cast<ProcessModule>().SingleOrDefault(m=>m.FileName.Equals(owner.FileName,StringComparison.OrdinalIgnoreCase)&&m.ModuleMemorySize==owner.ModuleMemorySize)
                ??throw new NotSupportedException("Game Windows API module does not match the local system module");
            return remote.BaseAddress+checked((int)(address.ToInt64()-owner.BaseAddress.ToInt64()));
        }
        [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] private static extern IntPtr GetModuleHandle(string name);
        [DllImport("kernel32.dll",CharSet=CharSet.Ansi)] private static extern IntPtr GetProcAddress(IntPtr module,string name);
        [DllImport("kernel32.dll")] private static extern bool FlushInstructionCache(IntPtr process,IntPtr address,UIntPtr length);
    }
}
