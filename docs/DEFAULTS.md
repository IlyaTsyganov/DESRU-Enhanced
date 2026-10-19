# Default DESRU command values

The source, application configuration, command help and update package share these defaults.

| Command | Default |
| --- | --- |
| `desru_speedometer_show` | `1` (both readings selected) |
| `desru_speedometer_showlanding` | `1` |
| `desru_speedometer_x_pos` | `0 current`, `100 landing` |
| `desru_speedometer_y_pos` | `480` for both |
| `desru_speedometer_size` | `3.5` for both |
| `desru_speedometer_color` | `255 255 255` for both |
| `desru_lowhptintopacity` | `100` |

Positions are physical pixels from the game's top-left corner. The positions migration is already marked complete for this default layout. Defaults apply to new or reset profiles; existing saved preferences remain authoritative. Query a command without arguments to see its current and default values.

The exact underlying application settings are in [DEFAULTS.json](DEFAULTS.json). No unrelated game cvars or personal profile data are included.
