def parse_kv(line):
    """Split KEY=VALUE on the FIRST equals sign only."""
    parts = line.split('=')
    return parts[0], parts[1]
