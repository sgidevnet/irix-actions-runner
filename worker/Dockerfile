FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libasound2t64 libstdc++6 ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/iris
COPY iris iris-ci entrypoint.sh ./
COPY worker.toml nvram.bin ./
COPY indy.chd indy.chd.diff.chd ./
RUN chmod +x iris iris-ci entrypoint.sh && mkdir -p /job
ENTRYPOINT ["/opt/iris/entrypoint.sh"]
