using DESpeedrunUtil.Overlay;
using System.Configuration;
using Settings = DESpeedrunUtil.Properties.Settings;

internal static class CommandDefaultsTests {
    internal static void Run(Action<bool, string> check) {
        var expected = new Dictionary<string, string> {
            ["CurrentSpeedOverlayEnabled"] = "True", ["LandingSpeedOverlayEnabled"] = "True", ["SpeedometersEnabled"] = "True",
            ["CurrentSpeedOverlayX"] = "0", ["CurrentSpeedOverlayY"] = "480", ["LandingSpeedOverlayX"] = "100", ["LandingSpeedOverlayY"] = "480",
            ["CurrentSpeedOverlayScale"] = "3.5", ["LandingSpeedOverlayScale"] = "3.5", ["SpeedometerPositionsVersion"] = "1",
            ["LowHpTintOpacity"] = "100", ["CurrentSpeedOverlayColor"] = "White", ["LandingSpeedOverlayColor"] = "White"
        };
        var settings = new Settings();
        check(expected.All(e => (string) settings.Properties[e.Key].DefaultValue == e.Value), "Compiled command defaults match the saved layout, visibility, colors and tint");
        var config = ConfigurationManager.OpenMappedExeConfiguration(new ExeConfigurationFileMap { ExeConfigFilename = typeof(Settings).Assembly.Location + ".config" }, ConfigurationUserLevel.None);
        var section = (ClientSettingsSection) config.GetSection("userSettings/DESpeedrunUtil.Properties.Settings");
        check(expected.All(e => section.Settings.Get(e.Key)?.Value.ValueXml.InnerText == e.Value), "Application config and compiled command defaults stay consistent");
        string[] values = { "1", "current=0 landing=100", "480", "3.5", "100", "1", "255 255 255" };
        check(values.Select((value, command) => SpeedometerCommandHelp.Format(settings, command).Contains("default:\"" + value + "\"")).All(ok => ok),
            "All seven command help queries report the promoted application defaults");
    }
}
