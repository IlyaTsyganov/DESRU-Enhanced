using DESpeedrunUtil.Overlay;
using System.Diagnostics;
using System.Reflection.PortableExecutable;
using System.Runtime.InteropServices;
using System.Text;
using System.Security.Cryptography;
using static DESpeedrunUtil.Interop.DLLImports;

namespace DESpeedrunUtil.Memory {
    internal readonly record struct VisualFeatureState(int Requested, int Active, int State, int Error, int Profile,
        long Presents, long Barriers, long Patched, long Draws, int Swapchains, int Hooks,float RenderDistance);

    internal sealed class LiveVisualFeatures : IDisposable {
        [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] private static extern IntPtr GetModuleHandle(string name);
        [DllImport("kernel32.dll",CharSet=CharSet.Ansi)] private static extern IntPtr GetProcAddress(IntPtr module,string name);
        private readonly Process _game;
        private readonly NativeSpeedometer _native;
        private readonly IntPtr _module, _control;
        private readonly Dictionary<string, int> _exports;
        private int _desired = -1;
        private float _distance=-1;
        private bool _registered, _refreshRequested, _started;
        internal VisualFeatureState State { get; private set; }
        internal bool CommandsRegistered => _registered;
        internal bool RuntimeStarted => _started;
        internal bool RefreshNeeded => (State.Requested & 2) != 0 && State.State == 2 && State.Swapchains == 0 && !_refreshRequested;

        private LiveVisualFeatures(Process game, NativeSpeedometer native, IntPtr module, Dictionary<string, int> exports) {
            _game = game; _native = native; _module = module; _exports = exports; _control = module + exports["DESRUVisualControl"];
            var magic = game.ReadBytes(_control, 12);
            if(magic == null || Encoding.ASCII.GetString(magic, 0, 8) != "DESRUV02" || BitConverter.ToInt32(magic, 8) != 2)
                throw new NotSupportedException("The loaded DESRU visual module has an incompatible control protocol");
        }
        internal static async Task<LiveVisualFeatures> AttachAsync(Process game, NativeSpeedometer native, string path, bool startRuntime = true) {
            path = Path.GetFullPath(path);
            var exports = ReadExports(path);
            // A loaded Windows DLL is locked. Stage immutable builds separately
            // so updating DESRU can reconnect without overwriting live code.
            string hash=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
            string staged=Path.Combine(Path.GetDirectoryName(path)!,"cache",hash,"DESRU.Visuals.Runtime.dll");
            Directory.CreateDirectory(Path.GetDirectoryName(staged)!);
            if(!File.Exists(staged)) File.Copy(path,staged);
            path=staged;
            game.Refresh();
            var loaded = game.Modules.Cast<ProcessModule>().FirstOrDefault(m => string.Equals(m.FileName,path,StringComparison.OrdinalIgnoreCase));
            IntPtr module = loaded?.BaseAddress ?? IntPtr.Zero;
            if(module == IntPtr.Zero) {
                byte[] text = Encoding.Unicode.GetBytes(path + '\0');
                IntPtr buffer = game.AllocateMemory(text.Length);
                if(buffer == IntPtr.Zero || !game.WriteBytes(buffer, text)) throw new InvalidOperationException("Cannot publish the visual module path");
                bool acknowledged = false;
                try {
                    IntPtr localLoad = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW");
                    using var local = Process.GetCurrentProcess();
                    var owner = local.Modules.Cast<ProcessModule>().Single(m => localLoad.ToInt64() >= m.BaseAddress.ToInt64() && localLoad.ToInt64() < m.BaseAddress.ToInt64() + m.ModuleMemorySize);
                    var remote = game.Modules.Cast<ProcessModule>().Single(m => m.ModuleName.Equals(owner.ModuleName, StringComparison.OrdinalIgnoreCase));
                    await native.Extension.InvokeAsync(remote.BaseAddress + checked((int) (localLoad.ToInt64() - owner.BaseAddress.ToInt64())), buffer);
                    acknowledged = true;
                    module = game.ReadPointer(native.Extension.State + 120);
                    if(module == IntPtr.Zero) throw new InvalidOperationException("The game could not load DESRU.Visuals.dll");
                } finally { if(acknowledged) game.FreeMemory(buffer); }
            }
            var result = new LiveVisualFeatures(game, native, module, exports);
            if(startRuntime) await result.StartRuntimeAsync();
            return result;
        }
        internal async Task StartRuntimeAsync() {
            if(_started) return;
            await _native.Extension.InvokeAsync(_module + _exports["DesruVisualsStart"]);
            _started = true;
        }

        internal static int Request(bool lighting, bool cheats, bool shown, bool fill, bool triggers, bool collision, bool walls) =>
            (lighting ? 1 : 0) | (cheats && shown ? 2 : 0) | (triggers ? 4 : 0) | (collision ? 8 : 0) |
            (walls ? 16 : 0) | (fill ? 32 : 0) | (cheats ? 64 : 0);

        internal void Update(int desired,float distance=40) {
            if(!float.IsFinite(distance)||distance<0) distance=40;
            if(_distance!=distance) {
                if(!_game.WriteBytes(_control+80,BitConverter.GetBytes(distance))) throw new InvalidOperationException("Cannot update collision viewer rendering distance");
                _distance=distance;
            }
            if(_desired != desired) {
                if(!_game.WriteBytes(_control + 12, BitConverter.GetBytes(desired))) throw new InvalidOperationException("Cannot update visual feature switches");
                _desired = desired;
            }
            if(!_game.WriteBytes(_control + 32, BitConverter.GetBytes(Environment.TickCount64 + 3000))) throw new InvalidOperationException("Cannot renew the visual module heartbeat");
            byte[] data = _game.ReadBytes(_control, 84) ?? throw new InvalidOperationException("Cannot read visual module status");
            State = new(BitConverter.ToInt32(data,12),BitConverter.ToInt32(data,16),BitConverter.ToInt32(data,20),BitConverter.ToInt32(data,24),BitConverter.ToInt32(data,28),
                BitConverter.ToInt64(data,40),BitConverter.ToInt64(data,48),BitConverter.ToInt64(data,56),BitConverter.ToInt64(data,64),BitConverter.ToInt32(data,72),BitConverter.ToInt32(data,76),BitConverter.ToSingle(data,80));
        }
        internal async Task RegisterCommandsAsync() {
            if(_registered) return;
            IntPtr system = _game.ReadPointer(_game.MainModule!.BaseAddress + _native.Layout.CommandGlobal);
            IntPtr add = _game.ReadPointer(_game.ReadPointer(system) + 24);
            byte[] request = BitConverter.GetBytes(system.ToInt64()).Concat(BitConverter.GetBytes(add.ToInt64()))
                .Concat(BitConverter.GetBytes((_game.MainModule.BaseAddress + _native.Layout.Print).ToInt64())).ToArray();
            IntPtr buffer = _game.AllocateMemory(request.Length);
            if(buffer == IntPtr.Zero || !_game.WriteBytes(buffer, request)) throw new InvalidOperationException("Cannot publish collision viewer console registration");
            bool acknowledged = false;
            try {
                int result = await _native.Extension.InvokeAsync(_module + _exports["DesruVisualsRegister"], buffer);
                acknowledged = true;
                if(result != 0) throw new InvalidOperationException("Cannot register collision viewer commands");
                _registered = true;
            } finally { if(acknowledged) _game.FreeMemory(buffer); }
        }
        internal async Task RefreshRendererAsync() {
            _refreshRequested = true;
            string mode=ReadVideoMode();
            try {
                // vid_restart alone preserves cached depth images. A temporary
                // mode change rebuilds them; restore the user's mode immediately.
                await _native.Extension.ExecuteAsync("r_mode "+(mode=="0"?"1":"0"));
                int before=State.Swapchains;
                await _native.Extension.ExecuteAsync("vid_restart");
                await WaitForSwapchainAsync(before);
            } catch { _refreshRequested=false;throw; }
            finally {
                await _native.Extension.ExecuteAsync("r_mode "+mode);
                int before=State.Swapchains;
                await _native.Extension.ExecuteAsync("vid_restart");
                await WaitForSwapchainAsync(before);
            }
        }
        private async Task WaitForSwapchainAsync(int before) {
            var watch=Stopwatch.StartNew();
            while(watch.ElapsedMilliseconds<10000) {
                Update(_desired,_distance);
                if(State.Swapchains>before) return;
                await Task.Delay(50);
            }
            throw new TimeoutException("The game did not rebuild graphics resources");
        }
        private string ReadVideoMode() {
            var layout=_native.Layout.Cvars??throw new NotSupportedException("Cannot resolve the current video mode");
            IntPtr system=_game.ReadPointer(_game.MainModule!.BaseAddress+layout.SystemGlobal),registry=_game.ReadPointer(system+8);
            IntPtr list=_game.ReadPointer(registry);int count=_game.ReadValue<int>(registry+8);
            if(count<1||count>65536) throw new InvalidOperationException("Invalid game cvar registry");
            string Text(IntPtr address)=>Encoding.UTF8.GetString(_game.ReadBytes(address,256)??throw new InvalidOperationException("Cannot read video mode")).Split('\0')[0];
            for(int i=0;i<count;i++) {
                IntPtr data=_game.ReadPointer(_game.ReadPointer(list+i*8));
                if(Text(_game.ReadPointer(data+layout.Name))!="r_mode") continue;
                string mode=Text(_game.ReadPointer(data+layout.ValueString));
                if(!int.TryParse(mode,out _)) throw new InvalidOperationException("Invalid video mode");
                return mode;
            }
            throw new NotSupportedException("The game video mode is unavailable");
        }
        public void Dispose() {
            try {
                if(!_game.HasExited) {
                    _game.WriteBytes(_control + 12, new byte[4]);
                    _game.WriteBytes(_control + 32, new byte[8]);
                }
            } catch { }
        }
        internal static Dictionary<string, int> ReadExports(string path) {
            byte[] bytes = File.ReadAllBytes(path);
            using var pe = new PEReader(new MemoryStream(bytes));
            var header = pe.PEHeaders.PEHeader ?? throw new NotSupportedException("Missing native visual module PE header");
            int Offset(int rva) {
                foreach(var section in pe.PEHeaders.SectionHeaders)
                    if(rva >= section.VirtualAddress && rva < section.VirtualAddress + section.SizeOfRawData)
                        return checked(rva - section.VirtualAddress + section.PointerToRawData);
                throw new NotSupportedException("Native visual export is outside the PE sections");
            }
            int directory = Offset(header.ExportTableDirectory.RelativeVirtualAddress);
            int functions = Offset(BitConverter.ToInt32(bytes,directory+28)), names = Offset(BitConverter.ToInt32(bytes,directory+32)), ordinals = Offset(BitConverter.ToInt32(bytes,directory+36));
            var exports = new Dictionary<string, int>();
            for(int i=0;i<BitConverter.ToInt32(bytes,directory+24);i++) {
                int text = Offset(BitConverter.ToInt32(bytes,names+4*i)), end = Array.IndexOf(bytes,(byte)0,text);
                string name = Encoding.ASCII.GetString(bytes,text,end-text);
                exports[name] = BitConverter.ToInt32(bytes,functions+4*BitConverter.ToUInt16(bytes,ordinals+2*i));
            }
            foreach(string name in new[] { "DESRUVisualControl", "DesruVisualsStart", "DesruVisualsRegister" })
                if(!exports.ContainsKey(name)) throw new NotSupportedException("Missing native visual export: " + name);
            return exports;
        }
    }
}
