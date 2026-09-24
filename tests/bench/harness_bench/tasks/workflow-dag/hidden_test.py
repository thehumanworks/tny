import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from swarm_cases import grade

name = "workflow"
result = grade(name, Path(sys.argv[1]))
assert result["passed"], result["failures"]
