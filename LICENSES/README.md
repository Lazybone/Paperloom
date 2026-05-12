# Third-Party Font Licenses

The reader font headers under `src/font_*.h` are auto-generated from the
following third-party fonts.  Each font is bundled under the SIL Open Font
License v1.1.  The full license text and upstream copyright lines are in
the per-family files in this directory.

| Family       | Role     | Upstream                                       | License | File                       |
|--------------|----------|------------------------------------------------|---------|----------------------------|
| Lexend Deca  | Reader   | https://github.com/googlefonts/lexend          | OFL-1.1 | `OFL-1.1-Lexend.txt`       |
| Literata     | Reader   | https://github.com/googlefonts/literata        | OFL-1.1 | `OFL-1.1-Literata.txt`     |
| Bitter Pro   | Reader   | https://github.com/solmatas/BitterPro (via google/fonts) | OFL-1.1 | `OFL-1.1-Bitter.txt`       |
| Inter        | UI chrome| https://github.com/rsms/inter (via google/fonts) | OFL-1.1 | `OFL-1.1-Inter.txt`        |
| ChareInk7SP  | Reader   | SIL Charis derivative via https://github.com/uxjulia/CrossInk | OFL-1.1 | `OFL-1.1-ChareInk.txt`     |

The firmware itself is MIT-licensed; see the top-level `LICENSE` for terms.

The font headers embed compressed 4-bpp glyph bitmaps generated once
from each upstream Regular face via `tools/fontconvert.py`.  The generated
headers are checked into `src/font_*.h` and are self-contained — no
network access, no TTF cache, and no regeneration are required to build
the firmware.  Re-run `tools/fontconvert.py` manually if you need to
refresh from a newer upstream release or alter the glyph size/range.
