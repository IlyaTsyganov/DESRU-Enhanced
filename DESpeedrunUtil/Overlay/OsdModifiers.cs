namespace DESpeedrunUtil.Overlay {
    internal static class OsdModifiers {
        internal static string Flags(bool macro, bool firewall, bool reshade, bool slopeboostDisabled, bool limiterDisabled, bool lighting) =>
            (macro ? "M" : "") + (firewall ? "F" : "") + (reshade ? "R" : "") + (slopeboostDisabled ? "S" : "") +
            (limiterDisabled ? "L" : "") + (lighting ? "X" : "");
        internal static string Version(string version, string flags) {
            version = version.Replace(" Rev ", "r").Replace(" (Gamepass)", "").Replace("1.0 (Release)", "Release");
            string suffix = flags.Length == 0 ? "" : " (" + flags + ")";
            // The game's shared metric string has room for 17 characters plus NUL.
            // Preserve every flag; abbreviate the version first if the row is full.
            int capacity = 17 - suffix.Length;
            if(version.Length > capacity && suffix.StartsWith(' ')) { suffix = suffix[1..]; capacity++; }
            if(version.Length > capacity) version = version[..capacity];
            return version + suffix;
        }
    }
}
