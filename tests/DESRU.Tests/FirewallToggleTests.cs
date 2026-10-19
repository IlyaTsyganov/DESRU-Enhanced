using DESpeedrunUtil;
using DESpeedrunUtil.Firewall;
using DESpeedrunUtil.Memory;
using System.Collections;
using System.Reflection;
using System.Runtime.Serialization;

internal static class FirewallToggleTests {
    // Explicit opt-in: uses temporary rules for nonexistent executables, never the game.
    internal static void Run(Action<bool, string> check) {
        string fixture = Path.Combine(Environment.CurrentDirectory, "artifacts", "firewall-test-" + Guid.NewGuid().ToString("N"));
        string main = Path.Combine(fixture, "main", "DOOMEternalx64vk.exe");
        string extra = Path.Combine(fixture, "extra", "DOOMEternalx64vk.exe");
        string other = Path.Combine(fixture, "other", "DOOMEternalx64vk.exe");
        string fresh = Path.Combine(fixture, "fresh", "DOOMEternalx64vk.exe");
        string sharedName = "DESRU firewall test " + Guid.NewGuid().ToString("N");
        dynamic policy = Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FwPolicy2")!)!;
        List<dynamic> Rules() => ((IEnumerable) policy.Rules).Cast<dynamic>()
            .Where(rule => ((string) rule.ApplicationName ?? "").StartsWith(fixture + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)).ToList();
        void Add(string application, int direction = 2, int action = 0) {
            dynamic rule = Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FWRule")!)!;
            rule.Name = sharedName; rule.ApplicationName = application;
            rule.Direction = direction; rule.Action = action; rule.Profiles = int.MaxValue; rule.Enabled = true;
            policy.Rules.Add(rule);
        }
        bool BlocksEnabled(string application, bool enabled) {
            var blocks = Rules().Where(rule => (string) rule.ApplicationName == application && (int) rule.Direction == 2 && (int) rule.Action == 0).ToList();
            return blocks.Count > 0 && blocks.All(rule => (bool) rule.Enabled == enabled);
        }
        try {
            Add(main); Add(main); Add(extra); Add(other); Add(main, action: 1); Add(main, direction: 1);
            check(Rules().Count == 6, "Fixture includes duplicate names, duplicate paths, an unrelated app, an allow and an inbound rule");
            check(FirewallHandler.CheckForFirewallRule(main.ToUpperInvariant()), "Firewall paths match without case sensitivity");

            var memory = (MemoryHandler) FormatterServices.GetUninitializedObject(typeof(MemoryHandler));
            using var window = new MainWindow();
            const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
            void Set(string name, object value) => typeof(MainWindow).GetField(name, flags)!.SetValue(window, value);
            Set("_gameDirectory", Path.GetDirectoryName(main)!);
            Set("_extraGameDirectories", new List<string> { Path.GetDirectoryName(extra)!, Path.GetDirectoryName(main)! });
            Set("<Memory>k__BackingField", memory);
            var button = (Button) window.Controls.Find("firewallToggleButton", true).Single();
            void Refresh() => typeof(MainWindow).GetMethod("RefreshFirewallStatus", flags)!.Invoke(window, null);
            void Click() => typeof(Button).GetMethod("OnClick", flags)!.Invoke(button, new object[] { EventArgs.Empty });
            Refresh();
            check(memory.GetFlag("firewall") && button.Text == "Disable Firewall Rule", "OS state drives both the button and HUD flag");
            Click();
            check(BlocksEnabled(main, false) && BlocksEnabled(extra, false), "One wired click disables all duplicates for both configured installations");
            check(BlocksEnabled(other, true) && Rules().Where(rule => (int) rule.Direction == 1 || (int) rule.Action == 1).All(rule => (bool) rule.Enabled),
                "Toggling preserves same-named rules for other apps, inbound rules and allow rules");
            Refresh(); Refresh();
            check(!FirewallHandler.CheckForFirewallRule(main) && BlocksEnabled(main, false), "Repeated status reads never re-enable a disabled rule or accept an allow/inbound rule");
            check(!memory.GetFlag("firewall") && button.Text == "Enable Firewall Rule", "Disabling clears F and updates the button");
            Click();
            check(BlocksEnabled(main, true) && BlocksEnabled(extra, true), "One wired click re-enables all matching blocks");
            check(memory.GetFlag("firewall") && button.Text == "Disable Firewall Rule", "Re-enabling restores F immediately");
            Click(); Click();
            check(Rules().Count == 6 && memory.GetFlag("firewall"), "Repeated off/on cycles preserve the rules and restore F");
            FirewallHandler.CreateFirewallRule(main, 0); FirewallHandler.CreateFirewallRule(main, 0);
            check(Rules().Count == 6, "Repeated setup does not add duplicate firewall rules");
            FirewallHandler.CreateFirewallRule(fresh, 0); FirewallHandler.CreateFirewallRule(fresh, 0);
            check(Rules().Count == 7 && FirewallHandler.CheckForFirewallRule(fresh), "A missing block is created once and reads back as enabled");
        } finally {
            // Remove only fixture objects, with unique names so COM's name-based Remove
            // cannot select a production rule that happened to share the original name.
            foreach(dynamic rule in Rules()) {
                string cleanupName = "DESRU firewall cleanup " + Guid.NewGuid().ToString("N");
                rule.Name = cleanupName;
                policy.Rules.Remove(cleanupName);
            }
            check(Rules().Count == 0, "All temporary firewall rules were removed");
        }
    }
}
