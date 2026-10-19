using DESpeedrunUtil.Memory;
using DESpeedrunUtil.Overlay;
using System.Diagnostics;
using System.Runtime.InteropServices;

internal static class ModbotInputTests {
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate void Menu(IntPtr self);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate int Event(IntPtr self,IntPtr value);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate short Keys(int key);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate ulong Clock();
    internal static void Run(Action<bool,string> check) {
        using var process=Process.GetCurrentProcess();
        IntPtr memory=process.AllocateMemory(16384);
        if(memory==IntPtr.Zero)throw new Exception("Cannot allocate modbot native test memory");
        IntPtr state=memory+4096,menu=memory+8192,tabs=memory+9216,root=memory+11264,other=memory+12288,evt=memory+14336;
        int shows=0,thinks=0,hides=0,events=0;bool e=false,q=false;ulong tick=1000;
        Menu show=_=>shows++,think=_=>thinks++,hide=_=>hides++;
        Event receive=(_,_)=>{events++;return 73;};
        Keys keys=key=>(short)((key==0x45&&e||key==0x51&&q)?short.MinValue:0);
        Clock clock=()=>tick;
        var layout=new NativeModbotInputLayout(0,0,0,0,0,0,0x188,0x88,0x10,8,0,0);
        try {
            IntPtr[] originals={Marshal.GetFunctionPointerForDelegate(show),Marshal.GetFunctionPointerForDelegate(think),Marshal.GetFunctionPointerForDelegate(receive),Marshal.GetFunctionPointerForDelegate(hide)};
            for(int i=0;i<4;i++)process.WriteBytes(memory+i*1024,NativeModbotInputCode.Create(i,state.ToInt64(),originals[i].ToInt64(),Marshal.GetFunctionPointerForDelegate(keys).ToInt64(),Marshal.GetFunctionPointerForDelegate(clock).ToInt64(),layout));
            process.WriteBytes(memory+2560,NativeModbotInputCode.Create(2,state.ToInt64(),originals[2].ToInt64(),Marshal.GetFunctionPointerForDelegate(keys).ToInt64(),Marshal.GetFunctionPointerForDelegate(clock).ToInt64(),layout with { NavigationEventFirst=52 }));
            process.VirtualProtect(memory,4096,MemPageProtect.PAGE_EXECUTE_READ);
            var onShow=Marshal.GetDelegateForFunctionPointer<Menu>(memory);
            var onThink=Marshal.GetDelegateForFunctionPointer<Menu>(memory+1024);
            var onEvent=Marshal.GetDelegateForFunctionPointer<Event>(memory+2048);
            var onHide=Marshal.GetDelegateForFunctionPointer<Menu>(memory+3072);
            Marshal.WriteIntPtr(menu,0x188,tabs);Marshal.WriteIntPtr(menu,0x88,root);Marshal.WriteIntPtr(tabs,0x10,root);
            Marshal.WriteInt32(state,8,1);Marshal.WriteInt64(state,16,10000);
            Marshal.WriteInt32(evt,8,51);
            onShow(menu);check(onEvent(tabs,evt)==73&&events==1,"Modbot navigation passes through when no opening key is held");
            e=true;onShow(menu);
            check(shows==2&&Marshal.ReadInt32(state,52)==1,"Native modbot show preserves the original and captures held E");
            for(int i=0;i<10;i++){onThink(menu);onEvent(tabs,evt);}
            check(events==1&&Marshal.ReadInt32(state,60)==10,"Repeated E opening events are suppressed for the entire hold");
            Marshal.WriteInt32(evt,8,52);onEvent(tabs,evt);check(events==1,"Previous-tab events are guarded as well");
            Marshal.WriteInt32(evt,8,0);check(onEvent(tabs,evt)==73&&events==2,"Non-navigation events are never swallowed");
            Marshal.WriteInt32(evt,8,51);check(onEvent(other,evt)==73&&events==3,"Other tab widgets are unaffected");
            Marshal.WriteIntPtr(tabs,0x10,other);onEvent(tabs,evt);check(events==4,"A reused or reparented widget cannot inherit modbot filtering");Marshal.WriteIntPtr(tabs,0x10,root);
            e=false;onThink(menu);onEvent(tabs,evt);check(events==4&&Marshal.ReadInt32(state,52)==2,"Queued key-release navigation is drained before unlocking tabs");
            e=true;onThink(menu);check(onEvent(tabs,evt)==73&&events==5,"The next fresh E press works after release and one UI update");
            q=true;onShow(menu);onEvent(tabs,evt);e=false;onThink(menu);onThink(menu);onEvent(tabs,evt);
            check(events==5&&Marshal.ReadInt32(state,48)==2,"Both captured opening keys must be released independently");
            q=false;onThink(menu);onThink(menu);check(onEvent(tabs,evt)==73&&events==6,"Q navigation resumes normally after release");
            q=true;onShow(menu);onHide(menu);check(hides==1&&onEvent(tabs,evt)==73&&Marshal.ReadIntPtr(state,24)==IntPtr.Zero,"Closing the modbot clears the latch without changing game input");
            onShow(menu);Marshal.WriteInt32(state,8,0);check(onEvent(tabs,evt)==73,"Disabling the guard immediately passes events through");
            Marshal.WriteInt32(state,8,1);onShow(menu);tick=10001;check(onEvent(tabs,evt)==73,"An expired DESRU lease cannot block navigation");onThink(menu);
            check(Marshal.ReadInt32(state,52)==0&&thinks>=16,"Expired lease clears native guard state while preserving Think calls");
            tick=1000;q=false;e=false;onShow(menu);check(onEvent(tabs,evt)==73,"Repeated openings without held keys remain unchanged");
            // The guard must not alter any game-owned fields used by this fixture.
            check(Marshal.ReadIntPtr(menu,0x188)==tabs&&Marshal.ReadIntPtr(tabs,0x10)==root,"Guard leaves game widgets and their enable/binding fields untouched");
            var steamEvent=Marshal.GetDelegateForFunctionPointer<Event>(memory+2560);
            q=true;onShow(menu);int beforeSteam=events;
            Marshal.WriteInt32(evt,8,51);check(steamEvent(tabs,evt)==73,"Steam's shifted COMMAND event is not mistaken for tab navigation");
            Marshal.WriteInt32(evt,8,52);steamEvent(tabs,evt);Marshal.WriteInt32(evt,8,53);steamEvent(tabs,evt);
            check(events==beforeSteam+1,"Steam TAB_NEXT/PREV values are resolved independently of legacy event numbers");
        } finally {
            process.FreeMemory(memory);GC.KeepAlive(show);GC.KeepAlive(think);GC.KeepAlive(hide);GC.KeepAlive(receive);GC.KeepAlive(keys);GC.KeepAlive(clock);
        }
    }
}
