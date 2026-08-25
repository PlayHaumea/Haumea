# Magnus PS5 core

This tree contains the PS5 emulation core used by the Magnus iPhoneOS app. Build it through the repository root; the shipping app target is `PS5`.

The core descends from [Kyty](https://github.com/InoriRus/Kyty). Kyty's MIT notice is preserved in `LICENSES/Kyty-MIT.txt`, and the Kyty-derived core keeps its GPL-2.0-or-later terms in `LICENSE`. Magnus integration files carry GPL-3.0-or-later SPDX headers.

Magnus replaces the x86-64 host path with the FEX source in `../FEX`. The app-facing entry points are in `src/ios`, and the Magnus-owned FEX adapter is in `../../Source/Stinger`.
