using NetFwTypeLib;
using Serilog;

namespace DESpeedrunUtil.Firewall {
    internal class FirewallHandler {

        private static readonly string FWRULE_NAME = "DOOMEternal (SR Utility)";

        /// <summary>
        /// Checks for an enabled outbound block without changing any rules.
        /// </summary>
        /// <param name="application">Full path of DOOMEternalx64vk.exe</param>
        /// <returns><see langword="true"/> if an enabled matching block is detected</returns>
        public static bool CheckForFirewallRule(string application) {
            INetFwPolicy2 policy2 = (INetFwPolicy2) Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FwPolicy2"));
            if(policy2 == null) {
                Log.Error("Firewall Policy was null. Aborting.");
                return false;
            }
            foreach(INetFwRule rule in policy2.Rules) {
                if(rule == null) {
                    Log.Error("Firewall rule was null. Aborting.");
                    return false;
                }
                if(IsApplicationBlock(rule, application) && rule.Enabled) return true;
            }
            return false;
        }

        /// <summary>
        /// Enables or disables every matching outbound block, including duplicates.
        /// Rule objects are updated directly because names can be shared by other installations.
        /// </summary>
        public static void SetFirewallRuleEnabled(string application, bool enabled, int num = 0) {
            INetFwPolicy2 policy2 = (INetFwPolicy2) Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FwPolicy2"));
            var matches = policy2.Rules.Cast<INetFwRule>().Where(rule => IsApplicationBlock(rule, application)).ToList();
            foreach(var rule in matches) {
                if(rule.Enabled == enabled) continue;
                rule.Enabled = enabled;
                Log.Information("Firewall rule {State}. path: {Path}", enabled ? "enabled" : "disabled", rule.ApplicationName);
            }
            if(enabled && matches.Count == 0) AddFirewallRule(policy2, application, num);
        }

        private static bool IsApplicationBlock(INetFwRule rule, string application) =>
            rule.Direction == NET_FW_RULE_DIRECTION_.NET_FW_RULE_DIR_OUT &&
            rule.Action == NET_FW_ACTION_.NET_FW_ACTION_BLOCK &&
            string.Equals(rule.ApplicationName, application, StringComparison.OrdinalIgnoreCase);

        /// <summary>
        /// Ensures an outbound firewall block exists without creating duplicates.
        /// </summary>
        /// <param name="application">Full path of DOOMEternalx64vk.exe</param>
        public static void CreateFirewallRule(string application, int num) {
            SetFirewallRuleEnabled(application, true, num);
        }

        private static void AddFirewallRule(INetFwPolicy2 policy2, string application, int num) {
            INetFwRule2 fwRule = (INetFwRule2) Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FWRule"));
            fwRule.Enabled = true;
            fwRule.Action = NET_FW_ACTION_.NET_FW_ACTION_BLOCK;
            fwRule.Direction = NET_FW_RULE_DIRECTION_.NET_FW_RULE_DIR_OUT;
            fwRule.Name = FWRULE_NAME;
            if(num > 0) fwRule.Name += " Extra " + num;
            fwRule.ApplicationName = application;
            fwRule.Profiles = (int) NET_FW_PROFILE_TYPE2_.NET_FW_PROFILE2_ALL;

            policy2.Rules.Add(fwRule);
            Log.Information("Firewall rule created. path: {Path}", fwRule.ApplicationName);
        }

    }
}
