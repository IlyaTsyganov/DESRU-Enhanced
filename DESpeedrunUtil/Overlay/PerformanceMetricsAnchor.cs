namespace DESpeedrunUtil.Overlay {
    internal readonly record struct PerformanceMetricsAnchor(float Right, float Top, float Scale) {
        internal static PerformanceMetricsAnchor BelowModifiers(Rectangle viewport, float virtualWidth, float virtualHeight, float glyphHeight, bool minimal) {
            // The metrics formatter uses a 10-unit right margin, y+4 and a
            // glyphHeight+2 row advance. DESRU keeps its modifiers in row 3 (row 1 in minimal OSD).
            int rows = minimal ? 1 : 3;
            float lastScale = minimal ? 1.75f : 1.25f;
            float top = 4 + (rows - 1) * (glyphHeight + 2) + glyphHeight * (lastScale + 0.25f);
            return new(viewport.Width - 10 * viewport.Width / virtualWidth, top * viewport.Height / virtualHeight, 0.65f);
        }
    }
}
