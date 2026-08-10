# SUMO plane benchmark

weft as the engine for a real traffic simulation. SUMO (Eclipse traffic
microsimulation) produces the workload: many vehicles as entities, one state frame
per simulation step. We use it to prove the benchmarks on real coherent movement
instead of synthetic random writes, and to compare the two wire formats (see
`../../Weft.Gateway`).

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
`../../Weft.Gateway`.

## Coupling the dense case with JuPedSim

SUMO models a pedestrian as a position on a stripe. That is the wrong model for a crowd,
so SUMO can hand the pedestrians inside a marked area to JuPedSim, which models them as
bodies in continuous space with contact forces.

`../../../docs/essays/sumo-traffic-sim.md` explains why weft cares and what it costs. This
section is the configuration.

Nothing here runs yet. The trace in this directory is vehicles only.

### The area

JuPedSim takes over inside a polygon, and the polygon is drawn by hand.

1. Open the network in `netedit`.
2. Network mode, then Polygons and Shapes.
3. Draw a polygon and set its type to `walkableArea`.
4. Stitch entry and exit edges along its boundary, so an agent can cross between the road
   network and the continuous area.

### The simulation

```xml
<configuration>
  <input>
    <net-file value="network.net.xml"/>
    <route-files value="demographics.rou.xml"/>
  </input>

  <processing>
    <pedestrian.model value="jupedsim"/>
    <pedestrian.jupedsim.steplen value="0.05"/>
    <pedestrian.jupedsim.strength-neighbor-repulsion value="2.5"/>
    <pedestrian.jupedsim.strength-geometry-repulsion value="5.0"/>
  </processing>

  <time>
    <begin value="0"/>
    <end value="3600"/>
    <!-- Contact forces need a short step. This is twenty steps for each simulated
         second, against one for the vehicle trace. -->
    <step-length value="0.05"/>
  </time>
</configuration>
```

### The people

```xml
<routes>
  <vType id="crowd" vClass="pedestrian"
         width="0.48" length="0.32" minGap="0.20" maxSpeed="1.35"/>

  <personFlow id="ingress" begin="0" end="1800" personsPerSecond="10">
    <walk edges="street_entry plaza_walkable_edge venue_exit"/>
  </personFlow>
</routes>
```

### Extracting frames

`extract_frames.py` reads the vehicle trace. A pedestrian carries a heading as well as a
position, because a body in continuous space has a facing and a vehicle on a lane does not.

```python
import traci

traci.start(["sumo", "-c", "sumo.cfg"])

while traci.simulation.getMinExpectedNumber() > 0:
    traci.simulationStep()

    frame = [
        {
            "id": pid,
            "x": round(x, 3),
            "y": round(y, 3),
            "angle": round(traci.person.getAngle(pid), 1),
            "speed": round(traci.person.getSpeed(pid), 2),
        }
        for pid in traci.person.getIDList()
        for (x, y) in [traci.person.getPosition(pid)]
    ]

    # One frame, the same shape replay.c reads.

traci.close()
```
