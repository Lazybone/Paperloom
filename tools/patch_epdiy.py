"""PlatformIO pre-build script — patches upstream vroland/epdiy v2.0.0
so that it coexists with an Arduino Wire instance that owns I²C-0.

Two transformations applied to `epd_board_v7.c::epd_board_init()`:

1. The standalone `ESP_ERROR_CHECK(i2c_param_config(...))` line is removed.
   Calling i2c_param_config on an already-installed driver re-programs the
   I²C peripheral registers without updating the driver's software cache,
   which silently corrupts subsequent transactions (devices on the bus
   stop ACKing for Wire even though epdiy itself still talks to PCA9555 +
   TPS65185 successfully).

2. The `ESP_ERROR_CHECK(i2c_driver_install(...))` line is replaced with a
   conditional block that:
     - tolerates ESP_FAIL and ESP_ERR_INVALID_STATE (driver already
       installed by Wire — both error codes are returned by various
       ESP-IDF versions for the same condition)
     - calls i2c_param_config ONLY when this is the first-time install
       (i.e. nobody else owns the driver yet)

The patch is idempotent and runs on every build, so it re-applies cleanly
after `pio pkg install` re-downloads the lib.
"""

import re
from pathlib import Path

Import("env")   # type: ignore[name-defined]  # provided by PlatformIO

TARGET_REL = "epdiy/src/board/epd_board_v7.c"

# v5 sentinel marker we leave in the patched file to detect re-runs.
# (V4 was a regression — it ran i2c_param_config on every boot, including
# the Wire-already-owns branch, which silently re-programmed the I2C
# peripheral underneath Wire and broke BQ27220 + GT911 transactions.
# We're back to V3's behaviour: param_config only on first-time install.)
SENTINEL = "/* PATCHED-EPDIY-V5 */"

# The original standalone param_config line.
PARAM_LINE_PATTERN = re.compile(
    r"[ \t]*ESP_ERROR_CHECK\(i2c_param_config\(EPDIY_I2C_PORT,\s*&conf\)\);\s*\n"
)

# The install line — either the original ESP_ERROR_CHECK form, or one of
# our prior-version patches that we need to overwrite.
INSTALL_PATTERN = re.compile(
    r"(?:"
    # Original
    r"ESP_ERROR_CHECK\(\s*i2c_driver_install\(([^)]+)\)\s*\);"
    r"|"
    # v1: only INVALID_STATE tolerated
    r"do \{ esp_err_t _e = i2c_driver_install\(([^)]+)\); "
    r"if \(_e != ESP_OK && _e != ESP_ERR_INVALID_STATE\) ESP_ERROR_CHECK\(_e\); "
    r"\} while \(0\);"
    r"|"
    # v2: INVALID_STATE + ESP_FAIL tolerated
    r"do \{ esp_err_t _e = i2c_driver_install\(([^)]+)\); "
    r"if \(_e != ESP_OK && _e != ESP_FAIL && _e != ESP_ERR_INVALID_STATE\) "
    r"ESP_ERROR_CHECK\(_e\); \} while \(0\);"
    r"|"
    # v3: param_config only on first-time install
    r"/\* PATCHED-EPDIY-V3 \*/ do \{ esp_err_t _e = i2c_driver_install\(([^)]+)\); "
    r"if \(_e == ESP_OK\) \{ ESP_ERROR_CHECK\(i2c_param_config\(EPDIY_I2C_PORT, &conf\)\); \} "
    r"else if \(_e != ESP_FAIL && _e != ESP_ERR_INVALID_STATE\) \{ ESP_ERROR_CHECK\(_e\); \} "
    r"\} while \(0\);"
    r"|"
    # v4: param_config always (broke Wire — reverted)
    r"/\* PATCHED-EPDIY-V4 \*/ do \{ esp_err_t _e = i2c_driver_install\(([^)]+)\); "
    r"if \(_e == ESP_OK \|\| _e == ESP_FAIL \|\| _e == ESP_ERR_INVALID_STATE\) "
    r"\{ ESP_ERROR_CHECK\(i2c_param_config\(EPDIY_I2C_PORT, &conf\)\); \} "
    r"else \{ ESP_ERROR_CHECK\(_e\); \} "
    r"\} while \(0\);"
    r")"
)


def _install_replacement(match: "re.Match[str]") -> str:
    args = (match.group(1) or match.group(2) or match.group(3)
            or match.group(4) or match.group(5))
    return (
        f"{SENTINEL} "
        "do { "
        f"esp_err_t _e = i2c_driver_install({args}); "
        # First-time install: apply our i2c_param_config now.
        "if (_e == ESP_OK) { "
        "ESP_ERROR_CHECK(i2c_param_config(EPDIY_I2C_PORT, &conf)); "
        "} "
        # Already installed (Wire owns it): don't re-config (re-running
        # i2c_param_config silently re-programs the peripheral and
        # breaks Wire's slave devices like BQ27220 + GT911 even though
        # the conf values are identical).
        "else if (_e != ESP_FAIL && _e != ESP_ERR_INVALID_STATE) { "
        "ESP_ERROR_CHECK(_e); "
        "} "
        "} while (0);"
    )


def patch_file(path: Path) -> None:
    if not path.is_file():
        print(f"[patch_epdiy] skip: {path} not present yet")
        return

    text = path.read_text()
    if SENTINEL in text:
        print(f"[patch_epdiy] already patched (v3): {path}")
        return

    # Remove the standalone param_config line (idempotent — zero or one match).
    text, n_param = PARAM_LINE_PATTERN.subn("", text)

    # Replace install line with conditional install + param_config block.
    text, n_install = INSTALL_PATTERN.subn(_install_replacement, text)

    if n_install == 0:
        print(f"[patch_epdiy] WARNING: install line not found in {path}")
        return

    path.write_text(text)
    print(f"[patch_epdiy] v3 patched: removed {n_param} param_config line(s), "
          f"replaced {n_install} install call(s) in {path}")


def main():
    project_dir = Path(env["PROJECT_DIR"])     # noqa: F821
    pio_env = env["PIOENV"]                    # noqa: F821
    target = project_dir / ".pio" / "libdeps" / pio_env / TARGET_REL
    patch_file(target)


main()
