FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libasound2t64 libstdc++6 ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/iris
# Coldest first: the disk image is 1.3 GB and the scripts change most often.
COPY indy.chd indy.chd.diff.chd ./
COPY iris iris-ci nvram.bin ./
# Hotter than the binaries: the snapshot must be retaken whenever either the
# emulator or indy.chd.diff.chd changes. The glob makes an absent saves/ a
# no-op, which is what leaves the entrypoint's cold-boot fallback reachable.
COPY save[s] ./saves
COPY worker.toml entrypoint.sh ./
RUN chmod +x iris iris-ci entrypoint.sh && mkdir -p /job
ENTRYPOINT ["/opt/iris/entrypoint.sh"]
