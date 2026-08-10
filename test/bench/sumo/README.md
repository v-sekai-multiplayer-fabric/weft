# SUMO plane benchmark

weft as the engine for a real traffic simulation. SUMO (Eclipse traffic
microsimulation) produces the workload: many vehicles as entities, one state frame
per simulation step. We use it to prove the benchmarks on real coherent movement
instead of synthetic random writes, and to compare the two wire formats (see
`../../docs/reference/protocol.md`).

## Setup

SUMO installs from the `eclipse-sumo` wheel, which ships prebuilt binaries plus the
`traci` control library.

```sh
python3 -m venv ../../.sumo-venv
. ../../.sumo-venv/bin/activate
pip install eclipse-sumo cbor2 zstandard
export SUMO_HOME=$(python -c "import os,sumo;print(os.path.dirname(sumo.__file__))")
export PATH="$SUMO_HOME/bin:$PATH"
export PYTHONPATH="$SUMO_HOME/tools:$PYTHONPATH"
```

## Generate the scenario

A 25 by 25 grid city with dense traffic. Peak about 8,600 concurrent vehicles.

```sh
cd scenario
netgenerate --grid --grid.number=25 --grid.length=200 \
  --default.lanenumber=2 --default.speed=13.9 -o grid.net.xml
python "$SUMO_HOME/tools/randomTrips.py" -n grid.net.xml -o trips2.xml \
  -r routes2.rou.xml -b 0 -e 600 -p 0.05 --fringe-factor 5 --validate
cd ..
```

## Extract the trace

Runs SUMO and dumps per-step vehicle positions as bitpacked frames (`frames.bin`).
Vehicle ids map to stable dense slots, so a slot is the same entity across frames.

```sh
cd scenario && python ../extract_frames.py && cd ..
```

## Prove the benchmarks

Nasty hot-path decode and apply in C, on the real trace:

```sh
cc -O3 -march=native -pthread replay.c -o replay
./replay scenario/frames.bin 1 40    # single core
./replay scenario/frames.bin 8 40    # eight cores
```

Cheap versus nasty wire size and encode/decode cost:

```sh
python encode_compare.py
```

## Result

840 M applies per second on one core (1.19 ns per apply), 56 times the 15 M packets
per second target, matching the synthetic `../pps_native.c`. Cheap CBOR JSON-LD is
2.3 times the raw bytes of nasty and 1.4 times after last-frame zstd. Full numbers in
`../../docs/reference/protocol.md`.
