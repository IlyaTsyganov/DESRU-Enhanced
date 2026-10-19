using DESpeedrunUtil.Memory;
using DESpeedrunUtil.Overlay;
using System.Diagnostics;

internal static class LiveModbotInputTests {
    internal static void Run(int seconds) {
        using var game=Process.GetProcessesByName("DOOMEternalx64vk").Single();
        // Borrow the running application's existing HUD state. Do not dispose that
        // shared adapter or change any meter, cheat, visual or FPS preferences.
        var native=NativeSpeedometer.Attach(game);
        using var guard=NativeModbotInputGuard.Attach(game,native.Extension,native.Layout);
        var timer=Stopwatch.StartNew();int lastShows=-1,lastEvents=-1,lastPhase=-1;
        Console.WriteLine("Modbot input guard active; FPS/cvars and game bindings unchanged.");
        try {
            while(timer.Elapsed.TotalSeconds<seconds&&!game.HasExited) {
                guard.Update(true);
                int shows=game.ReadValue<int>(guard.State+NativeModbotInputCode.Shows),events=guard.SuppressedEvents,phase=game.ReadValue<int>(guard.State+NativeModbotInputCode.Phase);
                if(shows!=lastShows||events!=lastEvents||phase!=lastPhase) {
                    Console.WriteLine($"guarded openings={shows}; blocked tab events={events}; phase={phase}");
                    lastShows=shows;lastEvents=events;lastPhase=phase;
                }
                Thread.Sleep(25);
            }
        } finally { Console.WriteLine("Temporary guard disabled; its callbacks remain safe pass-through code until game exit.");GC.KeepAlive(native); }
    }
}
