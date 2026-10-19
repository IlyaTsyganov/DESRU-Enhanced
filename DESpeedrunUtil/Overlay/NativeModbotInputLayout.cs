namespace DESpeedrunUtil.Overlay {
    internal sealed record NativeModbotInputLayout(int MenuVtable, int Show, int Hide, int Think,
        int TabsVtable, int Receive, int TabsOffset, int RootOffset, int ParentOffset, int EventTypeOffset,
        int AsyncKeyImport, int ClockImport) {
        public int ShowSlot { get; init; }=47;
        public int HideSlot { get; init; }=48;
        public int ThinkSlot { get; init; }=7;
        public int ReceiveSlot { get; init; }=10;
        public int NavigationEventFirst { get; init; }=51;
    }
}
