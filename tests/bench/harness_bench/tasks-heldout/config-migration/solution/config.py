import configparser
from pathlib import Path


def load_config(path):
    parser = configparser.ConfigParser()
    parser.read(path)
    storage = parser["storage"]
    value = storage.get("state_dir", fallback=storage.get("cache_dir"))
    if value is None:
        raise ValueError("missing state_dir")
    return {"state_dir": Path(value)}
