FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libasound2t64 libstdc++6 ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/iris
# Coldest first: the disk image is 1.3 GB and the scripts change most often.
COPY indy.chd indy.chd.diff.chd ./
# The snapshot and indy.chd.diff.chd are a matched pair: load_snapshot checks
# the SCSI id and size. saves/.cas is the bulk of it, a shared content-addressed
# store for the RAM chunks; saves/golden itself is a few megabytes.
COPY saves ./saves
COPY iris iris-ci nvram.bin ./
COPY worker.toml entrypoint.sh ./
RUN chmod +x iris iris-ci entrypoint.sh && mkdir -p /job
ENTRYPOINT ["/opt/iris/entrypoint.sh"]
