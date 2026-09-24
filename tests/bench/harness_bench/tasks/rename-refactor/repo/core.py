import json
from pathlib import Path


def load_profile(path):
    config = json.loads(Path(path).read_text())
    return Path(config["profile_path"]).read_text().strip()
