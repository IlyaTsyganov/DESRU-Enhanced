using DESpeedrunUtil.Memory;
using Microsoft.Win32.SafeHandles;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

internal static class VisualDepthDiagnostic {
    [DllImport("kernel32.dll",SetLastError=true)]
    private static extern SafeProcessHandle OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll",SetLastError=true)]
    private static extern bool ReadProcessMemory(SafeProcessHandle process,IntPtr address,[Out] byte[] bytes,nuint size,out nuint read);
    [DllImport("kernel32.dll",SetLastError=true)]
    private static extern bool WriteProcessMemory(SafeProcessHandle process,IntPtr address,byte[] bytes,nuint size,out nuint written);

    internal static void Run(string mode) {
        if(mode is not ("on" or "off" or "status")) throw new ArgumentException("Use on, off or status");
        using var game=Process.GetProcessesByName("DOOMEternalx64vk").Single();
        string path=Path.GetFullPath("output/visuals/DESRU.Visuals.Runtime.dll");
        string hash=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
        string staged=Path.Combine(Path.GetDirectoryName(path)!,"cache",hash,"DESRU.Visuals.Runtime.dll");
        var module=game.Modules.Cast<ProcessModule>().Single(m=>string.Equals(m.FileName,staged,StringComparison.OrdinalIgnoreCase));
        var exports=LiveVisualFeatures.ReadExports(path);
        // Request only VM_READ, plus VM_WRITE/VM_OPERATION for the session diagnostic.
        using var process=OpenProcess(mode=="status"?0x10u:0x38u,false,game.Id);
        if(process.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
        byte[] Read(IntPtr address,int count) {
            var bytes=new byte[count];
            if(!ReadProcessMemory(process,address,bytes,(nuint)count,out var read)||read!=(nuint)count)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return bytes;
        }
        byte[] data=Read(module.BaseAddress+exports["DESRUVisualControl"],84);
        if(Encoding.ASCII.GetString(data,0,8)!="DESRUV02"||BitConverter.ToInt32(data,8)!=2)
            throw new InvalidOperationException("Unexpected visual control protocol");
        IntPtr diagnostic=module.BaseAddress+exports["DESRUVisualDiagnosticSkipDepth"];
        int previous=BitConverter.ToInt32(Read(diagnostic,4));
        if(previous is not (0 or 1)) throw new InvalidOperationException("Unexpected diagnostic value");
        if(mode!="status") {
            if(mode=="off"&&(BitConverter.ToInt32(data,12)&2)==0) throw new InvalidOperationException("Enable Collision Viewer first");
            if(!WriteProcessMemory(process,diagnostic,BitConverter.GetBytes(mode=="off"?1:0),4,out var written)||written!=4)
                throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        Console.WriteLine(JsonSerializer.Serialize(new {Game=game.Id,
            DepthTest=BitConverter.ToInt32(Read(diagnostic,4))==0,Requested=BitConverter.ToInt32(data,12),
            Active=BitConverter.ToInt32(data,16),State=BitConverter.ToInt32(data,20),Error=BitConverter.ToInt32(data,24),
            Draws=BitConverter.ToInt64(data,64),Distance=BitConverter.ToSingle(data,80)}));
    }
}
