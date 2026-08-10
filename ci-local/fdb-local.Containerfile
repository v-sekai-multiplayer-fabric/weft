# Single-process FoundationDB server, local stand-in for a real FDB
# cluster (task #20's deployment needs one to be real, not a mock).
# Same FDB_VERSION real-build.yml and ci-local/Containerfile already
# install, same .deb packages, so this is the same FDB build the rest
# of this project is verified against, not a different one.
FROM docker.io/library/ubuntu:24.04

ARG FDB_VERSION=7.3.79
RUN apt-get update -qq && \
    apt-get install --no-install-recommends -qqy curl ca-certificates python3 && \
    curl -LSs "https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-clients_${FDB_VERSION}-1_amd64.deb" -o /tmp/fdb-client.deb && \
    curl -LSs "https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-server_${FDB_VERSION}-1_amd64.deb" -o /tmp/fdb-server.deb && \
    (dpkg -i /tmp/fdb-client.deb /tmp/fdb-server.deb || apt-get install -f -qqy) && \
    rm -f /tmp/fdb-*.deb && \
    rm -rf /var/lib/apt/lists/*

# The .deb's own postinst starts fdbmonitor via a systemd unit -- not
# usable as a container's own PID 1 without a systemd-in-container
# setup this does not need. Runs fdbserver directly in the foreground
# instead; entrypoint.sh writes the cluster file the first time it
# sees an empty datadir, then execs fdbserver so it stays PID 1 (real
# signal handling on `podman stop`, not a subshell wrapper).
COPY fdb-local-entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
