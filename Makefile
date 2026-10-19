.RECIPEPREFIX = >
.PHONY: engine venv

# Build the pinned engine (submodule) so the book matches OFI exactly
engine:
> cmake -S engine -B engine/build -G Ninja -DCMAKE_BUILD_TYPE=Release
> cmake --build engine/build

# Python environment for the oracle and research scripts
venv:
> python3 -m venv .venv
> .venv/bin/pip install -r requirements.txt
