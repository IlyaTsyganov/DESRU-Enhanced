namespace DESpeedrunUtil.Overlay {
    internal sealed record NativeTintLayout(int HudHook, int HudVtable, int SwfRootOffset, int VignetteHook,
        bool IndexedVignette, int VignetteBufferOffset, int OutputAlphaOffset, int PlayerVtable, int DeclVtable,
        int ScreenDeclOffset, int ScreenGroupsOffset, int HealthSlotOffset, int ActionsOffset, int ActionSize,
        int ActionTypeOffset, int ConditionsOffset, int VignetteActionType, int LowCondition, int VeryLowCondition);
}
