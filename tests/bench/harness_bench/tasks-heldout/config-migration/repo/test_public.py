from pathlib import Path

from config import load_config

assert load_config("configs/env_00.ini") == {"state_dir": Path("var/cache/00")}
