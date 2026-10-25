# jdm_shared

Shared components for the Equinox Audio plugin family (and Nova, the drum instrument):
[Equinox](https://github.com/JDMcE/Equinox) (EQ), [Meridian](https://github.com/JDMcE/Meridian)
(compressor), [Zenith](https://github.com/JDMcE/Zenith) (limiter) and
[Horizon](https://github.com/JDMcE/Horizon) (reverb). Each plugin expects this
repository to be checked out next to its own.

Header-only, namespace `jdm`, consumed as a CMake INTERFACE library (see
`CMakeLists.txt` header for wiring). Each plugin keeps thin forwarder headers
that alias `jdm` types into its own namespace and supply the product-specific
configuration:

| Component | Header | Product supplies |
|---|---|---|
| Design language / LookAndFeel | `jdm/ui/LookAndFeel.h` | palette pair (accent + reserved hue) via `colors::install` / the LookAndFeel constructor |
| Level + GR meters | `jdm/ui/Meters.h` | — (scales set at runtime) |
| Preset ecosystem | `jdm/presets/PresetManager.h` | `PresetProductConfig`: name, extension, XML tags, state version, inherited session data, session-only properties, factory table |
| Preset browser overlay | `jdm/ui/PresetBrowser.h` | — (reads the manager's config) |
| Snapshot A/B + undo/redo | `jdm/StateManager.h` | session root tag |
| EBU R128 momentary loudness | `jdm/dsp/LoudnessMeter.h` | — |
| Channel-strip DSP: biquads + channel EQ, compressor, lookahead limiter, gate (threshold or keyed), transient shaper, saturator, send reverb | `jdm/dsp/Biquad.h`, `ChannelEq.h`, `Compressor.h`, `Limiter.h`, `Gate.h`, `TransientShaper.h`, `Saturator.h`, `RoomReverb.h` | — (UI-less cores for mixers; extracted from Equinox, Meridian, Zenith and Horizon for Nova) |

The palette globals are process-wide *per plugin binary* — each product's DLL
carries its own copy, so sibling plugins in one host never fight over accents.

## License

Released under the GNU Affero General Public License v3.0 — see [LICENSE](LICENSE).
