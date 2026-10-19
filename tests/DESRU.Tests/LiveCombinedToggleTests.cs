using DESpeedrunUtil;
using DESpeedrunUtil.Memory;
using DESpeedrunUtil.Overlay;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.Serialization;

internal static class LiveCombinedToggleTests {
    internal static void Run(Action<bool, string> check) {
        using var game = Process.GetProcessesByName("DOOMEternalx64vk").Single();
        var layout = NativeGameLayout.Resolve(game.MainModule!.FileName);
        using var native = NativeSpeedometer.Attach(game);
        var monitor = new ProtectedCvarMonitor(game, layout.Cvars!);
        var runtime = new LiveMeathook(game, native.Extension, layout);
        var session = new CheatCvarSession(game, native.Extension, monitor);
        if(runtime.Active || session.Active) throw new InvalidOperationException("Leave the existing active cheat session untouched; this diagnostic requires cheats off");
        var initial = monitor.Read();
        if(initial.Any(c => c.Name == "timescale")) throw new InvalidOperationException("This diagnostic requires the default timescale");
        string volume = initial.Single(c => c.Name == "s_volume").Value;
        string source = Path.Combine(Environment.CurrentDirectory, "output/meath00k/XINPUT1_3.dll");
        string target = Path.Combine(AppContext.BaseDirectory, "meath00k/XINPUT1_3.dll");
        Directory.CreateDirectory(Path.GetDirectoryName(target)!); File.Copy(source, target, true);

        // Use the real form, wired button event and live controllers. A state-only
        // MemoryHandler avoids starting unrelated trainer/settings timers in the test.
        var memory = (MemoryHandler) FormatterServices.GetUninitializedObject(typeof(MemoryHandler));
        using var window = new MainWindow();
        const BindingFlags fields = BindingFlags.Instance | BindingFlags.NonPublic;
        void Set(string name, object value) => typeof(MainWindow).GetField(name, fields)!.SetValue(window, value);
        Set("<GameProcess>k__BackingField", game); Set("<Memory>k__BackingField", memory);
        Set("_nativeSpeedometer", native); Set("_cvarMonitor", monitor); Set("_liveMeathook", runtime); Set("_cheatCvarSession", session);
        window.Hooked = true;
        var button = (Button) window.Controls.Find("cheatsToggleButton", true).Single();
        typeof(MainWindow).GetMethod("UpdateLiveCheatsButton", fields)!.Invoke(window, null);
        var errors = new List<Exception>();
        ThreadExceptionEventHandler onError = (_, args) => errors.Add(args.Exception);
        Application.ThreadException += onError;
        var click = typeof(Button).GetMethod("OnClick", fields)!;
        bool Switching() => (bool) typeof(MainWindow).GetField("_switchingCheats", fields)!.GetValue(window)!;
        void Wait() {
            var timer = Stopwatch.StartNew();
            while(Switching() && timer.ElapsedMilliseconds < 30000) { Application.DoEvents(); Thread.Sleep(10); }
            if(Switching()) throw new TimeoutException("Combined toggle did not finish");
            Application.DoEvents();
        }
        void Click() { click.Invoke(button, new object[] { EventArgs.Empty }); Wait(); }
        try {
            check(button.Text == "Enable Cheats / Meath00k", "Unified button offers enabling both modes");
            click.Invoke(button, new object[] { EventArgs.Empty });
            check(!button.Enabled, "Unified button is disabled while switching");
            click.Invoke(button, new object[] { EventArgs.Empty }); Wait();
            check(memory.EnableCheats && memory.GetFlag("meath00k") && runtime.Active && session.Active, "One wired button click enables both modes; a repeated click while busy is ignored");
            check(button.Enabled && button.Text == "Disable Cheats / Meath00k" && button.ContextMenuStrip == null, "Enabled state offers one direct disable action without a context menu");
            Task.Run(() => native.Extension.ExecuteAsync("timescale 0.5")).GetAwaiter().GetResult();
            check(session.DisplayChanges(monitor.Read()).Any(c => c.Name == "timescale"), "New protected changes remain visible with the unified toggle");
            Click();
            check(!memory.EnableCheats && !memory.GetFlag("meath00k") && !runtime.Active && !session.Active, "One wired button click disables both modes");
            var restored = monitor.Read();
            check(!restored.Any(c => c.Name == "timescale") && restored.Single(c => c.Name == "s_volume").Value == volume, "Combined disable restores the test cvar and preserves initial volume");
            check(button.Enabled && button.Text == "Enable Cheats / Meath00k" && session.DisplayChanges(restored).Count == 0, "Disabled button and cvar display reflect the inactive state");
            Click(); Click();
            check(!runtime.Active && !memory.EnableCheats && !session.Active && errors.Count == 0, "Repeated combined switching completes without a disposed-menu or other unhandled UI exception");
        } finally {
            Application.ThreadException -= onError;
            if(runtime.Active && session.Active && !Switching()) {
                // Pump continuations captured by the real WinForms context.
                var restore = session.RestoreAsync();
                while(!restore.IsCompleted) { Application.DoEvents(); Thread.Sleep(10); }
                restore.GetAwaiter().GetResult(); runtime.Disable(); session.End();
            }
        }
    }
}
