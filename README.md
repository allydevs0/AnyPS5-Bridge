# AnyPS5-Bridge

Experimental fork of [AnyPS5](https://github.com/boykopovar/AnyPS5) focused on extended hardware compatibility, Vulkan/AGC rendering pipeline stability, and flexible input mapping.

Tools for automatic executable porting to Windows and Linux via direct relinking and native HLE PRX runtime libraries (no virtual machine or separate emulation layer).

[Usage](docs/user/USAGE.md) | [Build instructions](docs/dev/BUILD.md) | [Technical debt](docs/dev/TechnicalDebt.md) | [Code style](docs/dev/CONVENTIONS.md) | [Contributing](CONTRIBUTING.md)

---

## Fork Features & Status

### 1. Legacy CPU Compatibility (Non-AVX2 / Non-BMI2)
- **Target Architectures:** Full execution support on legacy x86-64 CPUs lacking AVX2/BMI1/BMI2/FMA (such as Intel Core 2nd/3rd Gen Sandy Bridge and Ivy Bridge, e.g. Core i5-3570K).
- **Relinker Compatibility Stubs & VEH Fallback:** Emits fast 128-bit decomposed vector stubs at relink time with an optimized VEH exception handler for runtime emulation, eliminating instruction faults (`#UD`).
- **ISA-Correct SSE Decomposition of AVX2 Broadcasts:** Ensures every translated `vbroadcast*` splat compiles to instructions the host CPU actually accepts. `vbroadcastsd`/`vbroadcastq` now decompose to `punpcklqdq` (`66 0F 6C`) instead of the AMD-only `movlpd xmm,xmm` (`66 0F 12`, an illegal-instruction trap on Intel), and `vbroadcastw`/`vbroadcastb` to `punpcklwd` (`66 0F 61`) instead of the saturating `packssdw` (`66 0F 6B`). This removes the illegal instruction (`#UD`) that previously crashed the game on the AVX2-to-native translation layer the moment gameplay's GPU path broadcast a float/double, letting non-AVX2 CPUs run the same accelerated code without faulting.

### 2. Vulkan / AGC Driver Rendering Fixes (`libSceAgcDriver`)
- **Dynamic UV Coordinate Sanitization:** Fixes collapsed UVs (`u2`/`v2` zeroed) in composite accumulation passes, fully restoring 16:9 widescreen presentation without vertical letterbox bars or squished graphics.
- **Hardware-Compliant Texture Channel Mapping:** Tolerates reserved/custom texture channel selectors (mapping GCN/RDNA `SEL_RESERVED` directly to standard Vulkan zero/one swizzles without dropping draw passes).
- **Matrix Buffer NaN/Inf Protection:** Guarantees stable projection and view transforms during runtime state transitions.

### 3. Tested Game Status

| Game | Title ID | Tested Platform | Hardware | Status | Performance |
|---|---|---|---|---|---|
| **Dreaming Sarah** | `PPSA02929` | Windows 10/11 x64 | Intel Core i5-3570K / GTX 1660 SUPER | **Playable** (Menus, Title, In-game) | **~58–60 FPS** (16:9 widescreen, pixel-perfect) |

---

## Input Mapping

- SDL-mapped controllers and keyboards are fully supported.
- Analog sticks, triggers, and digital pads map to `ScePad` states in real time.
- Configurable through `anyps5-input.ini` or direct input profiles. See [input mapping](docs/user/INPUT_MAPPING.md) for details.

---

## Disclaimer

This project is intended strictly for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

---

## License

This project is licensed under the GNU General Public License version 2 only (GPL-2.0). See [LICENSE](LICENSE) for details.

