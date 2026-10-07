#!/usr/bin/env python3
"""Emit a placeholder model_params.h so a fresh clone builds before training.

train.py overwrites this file with the real network. Keeping a checked-in
placeholder means `make` works on a machine with no Python at all, and makes
the generated-vs-hand-written boundary obvious in the diff.
"""
import pathlib, sys

CLASSES = ["baseline", "occupancy", "cooking", "ventilation", "volatile"]
N_FEAT, H1, H2 = 14, 16, 16


def main() -> int:
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                       else "core/include/aeris/model_params.h")
    nc = len(CLASSES)
    L = []
    a = L.append
    a("/* model_params.h - GENERATED. Do not edit by hand.")
    a(" * Placeholder produced by ml/bootstrap_params.py; run `make model` to")
    a(" * replace it with a trained network. The placeholder predicts class 0")
    a(" * unconditionally, which keeps the firmware building and the protocol")
    a(" * tests meaningful before any training has happened.")
    a(" */")
    a("#ifndef AERIS_MODEL_PARAMS_H")
    a("#define AERIS_MODEL_PARAMS_H")
    a("")
    a("#include <stdint.h>")
    a("")
    a('#define AERIS_MODEL_TRAINED 0')
    a('#define AERIS_MODEL_ID "placeholder"')
    a(f"#define AERIS_MODEL_H1 {H1}")
    a(f"#define AERIS_MODEL_H2 {H2}")
    a(f"#define AERIS_N_CLASSES {nc}")
    a("")
    a("#define AERIS_INPUT_SCALE  0.0625f")
    a("#define AERIS_OUTPUT_SCALE 0.0078125f")
    a("#define AERIS_M1_MULT 65536")
    a("#define AERIS_M1_SHIFT 7")
    a("#define AERIS_M2_MULT 65536")
    a("#define AERIS_M2_SHIFT 7")
    a("")
    a(f"static const float AERIS_FEAT_MEAN[{N_FEAT}] = {{"
      + ", ".join(["0.0f"] * N_FEAT) + "};")
    a(f"static const float AERIS_FEAT_INV_SCALE[{N_FEAT}] = {{"
      + ", ".join(["1.0f"] * N_FEAT) + "};")
    a("")
    a(f"static const int8_t AERIS_W1[{H1} * {N_FEAT}] = {{0}};")
    a(f"static const int32_t AERIS_B1[{H1}] = {{0}};")
    a(f"static const int8_t AERIS_W2[{H2} * {H1}] = {{0}};")
    a(f"static const int32_t AERIS_B2[{H2}] = {{0}};")
    a(f"static const int8_t AERIS_W3[{nc} * {H2}] = {{0}};")
    a(f"static const int32_t AERIS_B3[{nc}] = {{1, 0, 0, 0, 0}};")
    a("")
    a(f"static const char *const AERIS_CLASS_NAMES[{nc}] = {{"
      + ", ".join(f'"{c}"' for c in CLASSES) + "};")
    a("")
    a("#endif /* AERIS_MODEL_PARAMS_H */")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(L) + "\n")
    print(f"wrote placeholder {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
