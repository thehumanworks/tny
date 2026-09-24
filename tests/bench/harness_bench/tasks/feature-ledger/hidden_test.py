import concurrent.futures
import sys
import tempfile
import threading
from pathlib import Path

workspace = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(workspace))
sys.path.insert(0, str(Path(__file__).parent))
from swarm_cases import grade, load

name = "ledger"
result = grade(name, workspace)
assert result["passed"], result["failures"]

Ledger = load(workspace, "ledger").Ledger
for trial in range(2):
    with tempfile.TemporaryDirectory(prefix="ledger-race-") as tmp:
        path = Path(tmp) / "state.db"
        with Ledger(path):
            pass
        workers = 8
        rounds = 10
        start = threading.Barrier(workers)

        def writer(worker):
            with Ledger(path) as ledger:
                for index in range(rounds):
                    start.wait(timeout=15)
                    ledger.apply(f"event-{trial}-{index}-{worker}", "shared", 1)

        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            list(pool.map(writer, range(workers)))
        with Ledger(path) as ledger:
            assert ledger.snapshot()["shared"] == workers * rounds, (
                "lost concurrent updates",
                trial,
            )

        duplicate_start = threading.Barrier(workers)

        def duplicate(_worker):
            with Ledger(path) as ledger:
                duplicate_start.wait(timeout=15)
                return ledger.apply(f"duplicate-{trial}", "shared", 1)

        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            assert (
                list(pool.map(duplicate, range(workers)))
                == [workers * rounds + 1] * workers
            )
        with Ledger(path) as ledger:
            assert ledger.snapshot()["shared"] == workers * rounds + 1
