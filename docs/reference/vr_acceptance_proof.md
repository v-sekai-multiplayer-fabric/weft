# VR acceptance proof

Goal: prove we can serve at least 1000 HMDs with no motion sickness, with presence, and at
scale.

State: not started. It depends on #45 and the running pipeline. The headless client can
test the pipeline. Real VR presence needs a headset.

Next: define the acceptance test. Drive the SUMO playback through the interest feed to many
simulated HMD observers. Measure the latency and the frame budget.
