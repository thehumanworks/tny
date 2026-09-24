import json
from pathlib import Path


def read_profile(path):
    config = json.loads(Path(path).read_text())
    return Path(config["settings_path"]).read_text().strip()


def load_profile(path):
    # Deprecated alias.
    return read_profile(path)
