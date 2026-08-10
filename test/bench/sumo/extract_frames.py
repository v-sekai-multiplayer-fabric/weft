#!/usr/bin/env python3
# Run the SUMO scenario and dump per-step vehicle positions as a compact binary.
# Each vehicle is a weft entity; each simulation step is a state frame.
# Vehicle ids map to stable dense slots so a slot is the same entity across frames
# (this is what makes the last-frame zstd delta and cache locality realistic).
#
# frames.bin layout (little endian):
#   u32 magic  = 0x53554d4f ('SUMO')
#   u32 max_slots
#   u32 nframes
#   per frame: u32 active_count, then active_count * (u32 slot, f32 x, f32 y)
import os
import struct
import sys
import time

import traci
import traci.constants as tc

NET = "grid.net.xml"
ROUTES = "routes2.rou.xml"
END = 600
OUT = "frames.bin"

sumo_home = os.environ["SUMO_HOME"]
sumo_bin = os.path.join(sumo_home, "bin", "sumo")

traci.start([sumo_bin, "-n", NET, "-r", ROUTES, "--no-step-log",
             "--end", str(END), "--no-warnings"])

slot_of = {}
frames = []  # list of list[(slot, x, y)]
t0 = time.time()
step = 0
while traci.simulation.getMinExpectedNumber() > 0 and step < END:
    for vid in traci.simulation.getDepartedIDList():
        traci.vehicle.subscribe(vid, [tc.VAR_POSITION])
    res = traci.vehicle.getAllSubscriptionResults()
    frame = []
    for vid, sub in res.items():
        slot = slot_of.get(vid)
        if slot is None:
            slot = len(slot_of)
            slot_of[vid] = slot
        x, y = sub[tc.VAR_POSITION]
        frame.append((slot, x, y))
    frames.append(frame)
    traci.simulationStep()
    step += 1

traci.close()

max_slots = len(slot_of)
peak = max((len(f) for f in frames), default=0)
with open(OUT, "wb") as fh:
    fh.write(struct.pack("<III", 0x53554D4F, max_slots, len(frames)))
    for frame in frames:
        fh.write(struct.pack("<I", len(frame)))
        for slot, x, y in frame:
            fh.write(struct.pack("<Iff", slot, x, y))

dt = time.time() - t0
size = os.path.getsize(OUT)
print(f"frames={len(frames)} max_slots={max_slots} peak_active={peak} "
      f"file={size/1e6:.1f}MB extract={dt:.1f}s")
