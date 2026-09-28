import sys
from pathlib import Path

workspace = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(workspace))
sys.path.insert(0, str(Path(__file__).parent))
from swarm_cases import grade

name = "workflow"
result = grade(name, workspace)
assert result["passed"], result["failures"]
