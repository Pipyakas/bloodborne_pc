"""The FSR 4 DLL model's assets (tools/fsr4cap/build_assets.sh): where, which DLL, complete."""
import json
import os
from pathlib import Path
from bbport_i18n import tr

# fsr4_411 and BB_FSR411_DIR: the names before fsr4_model; still found.
FOLDERS = ("fsr4_dll", "fsr4_411")


def dll_model_dir(port_dir: Path, data_dir: Path, environ=os.environ) -> Path:
    """The folder the game uses (run.sh): an explicit variable, next to the port, the data dir."""
    for name in ("BB_FSR4_DLL_DIR", "BB_FSR411_DIR"):
        if environ.get(name):
            return Path(environ[name])
    for base in (port_dir, data_dir):
        for folder in FOLDERS:
            if (base / folder).is_dir():
                return base / folder
    return data_dir / FOLDERS[0]


def dll_model_version(directory: Path) -> str | None:
    """The source DLL's version from manifest.json; None without one (sets built before it)."""
    try:
        return json.loads((directory / "manifest.json").read_text()).get("upscaler_version")
    except (OSError, ValueError, AttributeError):
        return None


def dll_model_problem(directory: Path, output: str, preset: int) -> str | None:
    """Why the DLL model cannot run for this output and preset, or None when its set is complete."""
    w, h = map(int, output.split("x"))
    model = ("t2160" if w > 1920 or h > 1080 else "t1080") + ("_m1" if preset == 4 else "_m0")
    passes = ["spd", "prepass", "pass0_post"]
    for i in range(1, 13):
        passes.extend([f"pass{i}", f"pass{i}_post"])
    passes.extend(["postpass", "rcas"])
    for name in passes:
        path = directory / model / (name + ".spv")
        if not path.is_file() or path.stat().st_size < 20 or path.stat().st_size % 4:
            return tr("Нет или повреждён файл {}").format(f"{model}/{name}.spv")
    path = directory / model / "initializer.bin"
    if not path.is_file() or path.stat().st_size != 131072:
        return tr("Нет или повреждён файл {}").format(f"{model}/initializer.bin")
    return None
