import configparser
from pathlib import Path


def load_config(path):
    parser = configparser.ConfigParser()
    parser.read(path)
    return {"cache_dir": Path(parser["storage"]["cache_dir"])}
