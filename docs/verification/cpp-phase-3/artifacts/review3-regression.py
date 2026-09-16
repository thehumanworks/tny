import shlex
import subprocess
import tempfile
from pathlib import Path

root = Path.cwd()
directory = root / 'build/review3-regression'
directory.mkdir(exist_ok=True)
source = directory / 'runner-before.cpp'
source.write_bytes(subprocess.check_output(['git', 'show', 'HEAD:src/core/runner.cpp']))
values = subprocess.check_output(
    ['make', '-s', '-f', 'Makefile', '-f', '-', 'review-regression-vars'], text=True,
    input="review-regression-vars:\n\t@printf '%s\\n' '$(CXX)' '$(call cppflags,$(DBG_CFLAGS))' '$(filter-out $(CXX_RUNTIME),$(DBG_LDFLAGS))' '$(RUNNER_OWNERSHIP_OBJS)'\n"
).splitlines()
compiler, flags, linker, objects = map(shlex.split, values)
binary = directory / 'before'
subprocess.run([*compiler, *flags, f'-DTNY_RUNNER_SOURCE="{source}"', '-DTNY_ALLOC_TESTING=1',
                'tests/fixtures/runner_ownership.cpp', *objects, *linker, '-o', str(binary)], check=True)
with tempfile.TemporaryDirectory(prefix='tny-review-before-') as state:
    result = subprocess.run([str(binary), state], capture_output=True, text=True, timeout=60)
print(result.stdout + result.stderr)
assert result.returncode != 0 and 'yyjson_mut_is_bool(checkpoint_bool) && !yyjson_mut_get_bool(checkpoint_bool)' in result.stderr
print('PASS: checkpoint allocation regression rejects the rebased pre-fix runner at the cleared-boolean assertion before persistence.')
