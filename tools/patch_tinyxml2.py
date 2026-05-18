"""PlatformIO pre-build script — strips tinyxml2 test/contrib artifacts.

tinyxml2 ships two files at its repo root that each define their own
``main()`` function:

  * ``xmltest.cpp``           — the library's own unit-test runner
  * ``contrib/html5-printer.cpp`` — a sample application

pioarduino's stricter Library Dependency Finder (LDF) includes these
files when compiling the library, which causes a multiple-definition
link error against our own ``main.cpp``:

    multiple definition of `main`:
        tinyxml2/xmltest.cpp:301 vs tinyxml2/contrib/html5-printer.cpp:93

The fix is to delete the offending files before SCons starts its
compilation phase.  The script is idempotent: if the files are absent
(e.g. after a second build or a partial cache) it silently skips them.
Re-running ``pio pkg install`` will re-download them, but this pre-build
hook fires before any compilation, so they get removed again every time.
"""

from pathlib import Path

Import("env")   # type: ignore[name-defined]  # provided by PlatformIO

_TEST_FILES = [
    "xmltest.cpp",
    "contrib/html5-printer.cpp",
]


def main() -> None:
    lib_dir = (
        Path(env["PROJECT_LIBDEPS_DIR"])  # noqa: F821
        / env["PIOENV"]                   # noqa: F821
        / "tinyxml2"
    )

    if not lib_dir.is_dir():
        print("[patch_tinyxml2] tinyxml2 lib dir not found — skipping")
        return

    for rel in _TEST_FILES:
        f = lib_dir / rel
        if f.exists():
            f.unlink()
            print(f"[patch_tinyxml2] removed {f.relative_to(lib_dir.parent)}")
        else:
            print(f"[patch_tinyxml2] already absent: {rel}")


main()
