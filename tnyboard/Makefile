PYTHON ?= python3
RUFF ?= ruff

.PHONY: test lint format check install

test:
	cd .. && $(PYTHON) -m unittest discover -s tnyboard/tests -v

lint:
	$(RUFF) check .
	$(RUFF) format --check .

format:
	$(RUFF) format .

check: test lint

install:
	$(PYTHON) -m pip install .
