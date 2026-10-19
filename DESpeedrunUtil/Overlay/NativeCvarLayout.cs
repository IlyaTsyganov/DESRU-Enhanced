namespace DESpeedrunUtil.Overlay {
    internal sealed record NativeCvarLayout(int SystemGlobal, int DataSize, int ValueString, int Integer, int Float,
        int Name, int Reset, int Flags, int Cheat, int NoCheat, int ReadOnly, int InitOnly, int ShippingDisabled);
}
