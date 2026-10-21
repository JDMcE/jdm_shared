# jdm_shared

Shared source code for the **Equinox Audio** plugin family. This repository is for
developers building the plugins from source. It is not a plugin itself.

**Looking for the plugins?** Download them from their own pages:

| Plugin | What it is |
|---|---|
| [Equinox](https://github.com/JDMcE/Equinox) | 24-band parametric / dynamic EQ |
| [Meridian](https://github.com/JDMcE/Meridian) | Compressor with 14 styles |
| [Zenith](https://github.com/JDMcE/Zenith) | True peak mastering limiter |
| [Horizon](https://github.com/JDMcE/Horizon) | Algorithmic reverb, stereo to 9.1.6 |

---

## For developers

Each plugin expects this repository to be checked out next to its own:

```
some-folder/
  Equinox/
  jdm_shared/
```

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

The palette globals are process-wide *per plugin binary* — each product's DLL
carries its own copy, so sibling plugins in one host never fight over accents.

## License

Released under the GNU Affero General Public License v3.0 — see [LICENSE](LICENSE).
