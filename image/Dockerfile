FROM ubuntu:24.04
RUN sed -i 's|//.*archive.ubuntu.com|//mirrors.cloud.aliyuncs.com|g' /etc/apt/sources.list.d/ubuntu.sources \
 && apt-get update && apt-get install -y --no-install-recommends fuse3 \
 && rm -rf /var/lib/apt/lists/*
COPY jfs /usr/local/bin/juicefs
COPY mount.juicefs /bin/mount.juicefs
RUN chmod +x /usr/local/bin/juicefs /bin/mount.juicefs \
 && ln -s /usr/local/bin/juicefs /sbin/mount.juicefs \
 && mkdir -p /var/lib/jfs /root/.jfs-conf /var/jfsCache
