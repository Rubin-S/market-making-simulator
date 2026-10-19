.RECIPEPREFIX = >
.PHONY: engine venv sim test

# Build the pinned engine (submodule) so the book matches OFI exactly
engine:
> cmake -S engine -B engine/build -G Ninja -DCMAKE_BUILD_TYPE=Release
> cmake --build engine/build

# Python environment for the oracle and research scripts
venv:
> python3 -m venv .venv
> .venv/bin/pip install -r requirements.txt

# Build the simulator and its tests
sim:
> cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
> cmake --build build

# Run every C++ unit test
test: sim
> ctest --test-dir build --output-on-failure
