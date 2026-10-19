using DESpeedrunUtil.Memory;
using DESpeedrunUtil.Overlay;
using System.Diagnostics;
using System.Text.Json;

internal static class LiveVisualTests {
    internal static async Task Run(bool viewer,Action<bool,string> check) {
        using var game=Process.GetProcessesByName("DOOMEternalx64vk").Single();
        using var native=NativeSpeedometer.Attach(game);
        using var visuals=await LiveVisualFeatures.AttachAsync(game,native,Path.GetFullPath("output/visuals/DESRU.Visuals.Runtime.dll"));
        int desired=viewer?127:125;
        visuals.Update(desired);
        long initialPresents=visuals.State.Presents;
        var watch=Stopwatch.StartNew();
        while(watch.ElapsedMilliseconds<30000) {
            visuals.Update(desired);
            if(visuals.State.State<0) throw new InvalidOperationException(JsonSerializer.Serialize(visuals.State));
            if(visuals.State.State==2&&visuals.State.Presents>initialPresents+5) break;
            await Task.Delay(100);
        }
        Console.WriteLine(JsonSerializer.Serialize(visuals.State));
        check(visuals.State.State==2&&visuals.State.Hooks>0&&visuals.State.Presents>5,"The native bridge attaches to the existing game device and sees live presents");
        check((visuals.State.Active&1)!=0,"Lighting fix activates without restarting the game");
        if(viewer) {
            await visuals.RegisterCommandsAsync();
            if(visuals.RefreshNeeded) await visuals.RefreshRendererAsync();
            Console.WriteLine("Viewer observation starts");
            if(Environment.GetEnvironmentVariable("DESRU_TEST_OBSERVE")=="1") {
                for(int i=0;i<900;i++) { visuals.Update(desired);await Task.Delay(100); }
                Console.WriteLine("Observed: "+JsonSerializer.Serialize(visuals.State));
            }
            watch.Restart();
            while(watch.ElapsedMilliseconds<20000) {
                visuals.Update(desired);
                if(visuals.State.Error!=0) throw new InvalidOperationException(JsonSerializer.Serialize(visuals.State));
                if((visuals.State.Active&2)!=0) break;
                await Task.Delay(100);
            }
            Console.WriteLine(JsonSerializer.Serialize(visuals.State));
            check((visuals.State.Active&2)!=0,"Collision viewer activates in the same game process");
            watch.Restart();long startDraws=visuals.State.Draws;
            while(watch.ElapsedMilliseconds<20000&&visuals.State.Draws<=startDraws) { visuals.Update(desired);await Task.Delay(100); }
            check(visuals.State.Draws>startDraws,"Collision geometry is actually drawn in the active game level");
            foreach(string command in new[]{"desru_triggers_show","desru_collision_show","desru_invisible_walls_show"}) await native.Extension.ExecuteAsync(command+" 0");
            await Task.Delay(100);visuals.Update(desired);
            Console.WriteLine("Categories off: "+JsonSerializer.Serialize(visuals.State));
            check((visuals.State.Requested&28)==0,"All three console commands switch categories off");
            foreach(string command in new[]{"desru_triggers_show","desru_collision_show","desru_invisible_walls_show"}) await native.Extension.ExecuteAsync(command+" 1");
            await Task.Delay(100);visuals.Update(desired);
            check((visuals.State.Requested&28)==28,"All three console commands switch categories on");
            await native.Extension.ExecuteAsync("desru_volumes_render_distance 73.5");
            await Task.Delay(100);visuals.Update(desired);
            check(visuals.State.RenderDistance==73.5f,"Rendering distance accepts arbitrary fractional metres");
            foreach(string invalid in new[]{"-1","nan","25junk"}) await native.Extension.ExecuteAsync("desru_volumes_render_distance "+invalid);
            await Task.Delay(100);visuals.Update(desired);
            check(visuals.State.RenderDistance==73.5f,"Invalid distances do not change the current value");
            await native.Extension.ExecuteAsync("desru_volumes_render_distance 40");
            visuals.Update(126);await Task.Delay(100);visuals.Update(126);
            check(visuals.State.Active==2,"Viewer remains active with lighting fix disabled");
            visuals.Update(125);await Task.Delay(100);visuals.Update(125);
            check(visuals.State.Active==1,"Lighting fix remains active with viewer disabled");
            int swapchains=visuals.State.Swapchains;
            visuals.Update(127);await Task.Delay(100);visuals.Update(127);
            check(visuals.State.Active==3&&visuals.State.Swapchains==swapchains,"Re-enabling effects reuses graphics resources without another renderer refresh");
        }
        visuals.Update(60);await Task.Delay(500);visuals.Update(60);
        check(visuals.State.Active==0,"Both visual effects disable live");
        await visuals.RegisterCommandsAsync();
        await native.Extension.ExecuteAsync("desru_triggers_show 0");
        await native.Extension.ExecuteAsync("desru_volumes_render_distance 99");
        await Task.Delay(100);visuals.Update(60);
        check((visuals.State.Requested&4)!=0,"Viewer commands are blocked while cheats are disabled");
        check(visuals.State.RenderDistance==40,"Distance command is blocked while cheats are disabled");
        Console.WriteLine(JsonSerializer.Serialize(visuals.State));
    }
}
