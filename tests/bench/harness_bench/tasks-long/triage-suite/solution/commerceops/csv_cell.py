import csv
import io


def encode(value):
    """Encode one RFC 4180-style CSV cell."""
    buffer = io.StringIO()
    csv.writer(buffer, lineterminator="").writerow([value])
    return buffer.getvalue()
