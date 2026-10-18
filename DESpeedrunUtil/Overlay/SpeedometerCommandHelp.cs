using System.ComponentModel;
using System.Configuration;
using System.Globalization;
using Settings = DESpeedrunUtil.Properties.Settings;

namespace DESpeedrunUtil.Overlay {
    internal static class SpeedometerCommandHelp {
        internal static string Format(Settings settings, int command) {
            if(command < 0 || command >= NativeSpeedometerCode.Commands.Length) throw new ArgumentOutOfRangeException(nameof(command));
            // The settings provider can mutate the cached section while saving user settings.
            // Parse only the assembly's on-disk application config, never that shared cache.
            var config = ConfigurationManager.OpenMappedExeConfiguration(new ExeConfigurationFileMap {
                ExeConfigFilename = typeof(Settings).Assembly.Location + ".config"
            }, ConfigurationUserLevel.None);
            var applicationDefaults = config.GetSection("userSettings/DESpeedrunUtil.Properties.Settings") as ClientSettingsSection;
            string Value(string name, bool defaults) {
                var property = settings.Properties[name];
                object value = settings[name];
                if(defaults) {
                    // App defaults can differ from property attributes in a prepared distribution.
                    // This section is the application config, not the per-user settings file.
                    string raw = applicationDefaults?.Settings.Get(name)?.Value.ValueXml.InnerText ?? Convert.ToString(property.DefaultValue, CultureInfo.InvariantCulture)!;
                    value = TypeDescriptor.GetConverter(property.PropertyType).ConvertFromInvariantString(raw)!;
                }
                if(value is bool flag) return flag ? "1" : "0";
                if(value is Color color) return $"{color.R} {color.G} {color.B}";
                return Convert.ToString(value, CultureInfo.InvariantCulture)!;
            }
            string Pair(string suffix, bool defaults) {
                string current = Value("CurrentSpeedOverlay" + suffix, defaults), landing = Value("LandingSpeedOverlay" + suffix, defaults);
                return current == landing ? current : $"current={current} landing={landing}";
            }
            string Read(bool defaults) => command switch {
                0 => Value("SpeedometersEnabled", defaults),
                1 => Pair("X", defaults),
                2 => Pair("Y", defaults),
                3 => Pair("Scale", defaults),
                4 => Value("LowHpTintOpacity", defaults),
                5 => Value("LandingSpeedOverlayEnabled", defaults),
                _ => Pair("Color", defaults)
            };
            string description = command switch {
                0 => "Show or hide the speedometers (0/1). Showing enables current speed and preserves the landing preference. Optional [current|landing] changes only that reading.",
                1 => "Set horizontal position in physical pixels from the left edge. Applies to both speedometers unless [current|landing] is specified.",
                2 => "Set vertical position in physical pixels from the top edge. Applies to both speedometers unless [current|landing] is specified.",
                3 => "Set text size (0.1 to 10). Always applies to both speedometers.",
                4 => "Set low-health red tint opacity (0 to 100). 0 removes both layers; 100 restores original opacity.",
                5 => "Include or hide last-landing speed (0/1). Does not change master visibility or current speed.",
                _ => "Set text color using three integer RGB channels (0 to 255). Applies to both speedometers unless [current|landing] is specified."
            };
            return $"\"{NativeSpeedometerCode.Commands[command]}\" is:\"{Read(false)}\" default:\"{Read(true)}\"\n{description}\n";
        }
    }
}
