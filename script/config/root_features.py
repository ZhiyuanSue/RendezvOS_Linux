import json
import os
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: root_features.py <root_dir> <root_config_json>")
        return 2

    root_dir = sys.argv[1]
    cfg_path = sys.argv[2]
    out_path = os.path.join(root_dir, "Makefile.root.env")

    try:
        with open(cfg_path, "r") as f:
            cfg = json.load(f)
    except OSError:
        print(f"ERROR: cannot read root config: {cfg_path}")
        return 2

    features = cfg.get("features", [])
    use = cfg.get("use", True)
    if not isinstance(use, bool):
        print("ERROR: root.json 'use' must be a boolean")
        return 2

    if not isinstance(features, list):
        print("ERROR: root.json 'features' must be a list")
        return 2

    # Root-only extra CFLAGS for building linux_layer/servers at repo root.
    # Intentionally does NOT affect core kernel/module builds.
    if not use:
        line = "ROOT_EXTRA_CFLAGS +=\n"
    else:
        line = "ROOT_EXTRA_CFLAGS +=" + "".join([f" -D {f}" for f in features]) + "\n"

    try:
        with open(out_path, "w") as f:
            f.write(line)
    except OSError:
        print(f"ERROR: cannot write {out_path}")
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

