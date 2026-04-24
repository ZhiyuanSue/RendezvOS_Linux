import json
import os
import sys
from typing import Dict, List, Set


def _die(msg: str, code: int = 2) -> int:
    print(f"ERROR: {msg}")
    return code


def _read_json(path: str) -> Dict:
    with open(path, "r") as f:
        return json.load(f)


def _ensure_list_str(v, field: str) -> List[str]:
    if v is None:
        return []
    if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
        raise ValueError(f"{field} must be a list of strings")
    return v


def _core_kernel_features(core_cfg: Dict) -> List[str]:
    k = core_cfg.get("kernel", {})
    feats = k.get("features", [])
    if not isinstance(feats, list):
        return []
    out = []
    for f in feats:
        if isinstance(f, str) and f.strip():
            out.append(f.strip())
    return out


def _inherit_sets_from_core(core_cfg: Dict, sets: List[str]) -> List[str]:
    """
    Inheritance policy (keep root clean):
    - arch: only arch identity macros like _X86_64_/_AARCH64_/_RISCV64_/_LOONGARCH_
    - smp: SMP if present
    - dbg: intentionally empty for now (DBG is a Make var, not a macro)
    """
    want: Set[str] = set(sets)
    core_feats = _core_kernel_features(core_cfg)
    inherited: List[str] = []

    if "arch" in want:
        for f in core_feats:
            if f in ("_X86_64_", "_AARCH64_", "_RISCV64_", "_LOONGARCH_"):
                inherited.append(f)

    if "smp" in want:
        for f in core_feats:
            if f == "SMP":
                inherited.append(f)

    # "dbg" intentionally not inherited as a macro (see docstring).
    return inherited


def _mk_define_flags(macros: List[str]) -> str:
    return "".join([f" -D {m}" for m in macros])


def _mk_undef_flags(macros: List[str]) -> str:
    return "".join([f" -U {m}" for m in macros])


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: configure_root.py <root_dir> <arch>")
        return 2

    root_dir = sys.argv[1]
    arch = sys.argv[2]

    root_cfg_path = os.path.join(root_dir, "script", "config", f"root_{arch}.json")
    core_cfg_path = os.path.join(
        root_dir, "core", "script", "config", f"config_{arch}.json"
    )
    out_path = os.path.join(root_dir, "Makefile.root.env")

    if not os.path.isfile(root_cfg_path):
        return _die(f"root config not found: {root_cfg_path}")

    try:
        root_cfg = _read_json(root_cfg_path)
    except Exception as e:
        return _die(f"cannot read root config {root_cfg_path}: {e}")

    use = root_cfg.get("use", True)
    if not isinstance(use, bool):
        return _die("root config 'use' must be boolean")

    root_features = _ensure_list_str(root_cfg.get("root_features", []), "root_features")

    inherit_cfg = root_cfg.get("inherit_from_core", {})
    inherit_use = inherit_cfg.get("use", True)
    if not isinstance(inherit_use, bool):
        return _die("inherit_from_core.use must be boolean")
    inherit_sets = _ensure_list_str(inherit_cfg.get("sets", []), "inherit_from_core.sets")

    core_over = root_cfg.get("core_overrides", {})
    undef_features = _ensure_list_str(core_over.get("undef_features", []), "core_overrides.undef_features")
    extra_features = _ensure_list_str(core_over.get("extra_features", []), "core_overrides.extra_features")

    inherited: List[str] = []
    if use and inherit_use and inherit_sets:
        if os.path.isfile(core_cfg_path):
            try:
                core_cfg = _read_json(core_cfg_path)
                inherited = _inherit_sets_from_core(core_cfg, inherit_sets)
            except Exception as e:
                return _die(f"cannot read core config for inheritance {core_cfg_path}: {e}")
        else:
            return _die(f"core config not found for inheritance: {core_cfg_path}")

    # Root CFLAGS: only affects linux_layer/servers compilation at repo root.
    if not use:
        root_line = "ROOT_EXTRA_CFLAGS +=\n"
    else:
        all_root_defs = inherited + root_features
        root_line = "ROOT_EXTRA_CFLAGS +=" + _mk_define_flags(all_root_defs) + "\n"

    # Core overrides: only affects core compilation via EXTRA_CFLAGS in top Makefile.
    core_override_line = (
        "CORE_OVERRIDE_CFLAGS +="
        + _mk_undef_flags(undef_features)
        + _mk_define_flags(extra_features)
        + "\n"
    )

    try:
        with open(out_path, "w") as f:
            f.write(root_line)
            f.write(core_override_line)
    except OSError as e:
        return _die(f"cannot write {out_path}: {e}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

