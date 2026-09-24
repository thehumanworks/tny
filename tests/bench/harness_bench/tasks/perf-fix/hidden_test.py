import subprocess
import sys
from pathlib import Path

w = Path(sys.argv[1])
code = 'from records import dedupe_records\nassert dedupe_records([" X ","x",""," ","Straße","STRASSE"])==[" X ","","Straße"]\nrows=[f"name-{i:05d}" for i in range(30000)]+[f" NAME-{i:05d} " for i in range(30000)]\nassert dedupe_records(rows)==rows[:30000]\n'
subprocess.run(
    [sys.executable, "-c", code],
    cwd=w,
    check=True,
    timeout=7,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
subprocess.run(
    [sys.executable, "test_records.py"],
    cwd=w,
    check=True,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
