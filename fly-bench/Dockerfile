# Tiny image: just the C UDP packet-rate benchmark. Two machines of this measure
# the real-NIC receive ceiling over Fly's private 6PN network.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev && rm -rf /var/lib/apt/lists/*
COPY netbench.c /netbench.c
RUN gcc -O2 -pthread -o /netbench /netbench.c
FROM debian:bookworm-slim
COPY --from=build /netbench /usr/local/bin/netbench
# Idle by default; the server/client are launched explicitly via `fly ssh console`
# so a machine never bills doing nothing unexpected.
CMD ["sleep", "infinity"]
