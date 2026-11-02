# JuiceFS idmap + CSI 快速搭建与验证教程

> 目标：在一台 Ubuntu 24.04 机器上，从零搭建 k3s + juicefs-csi-driver 环境，挂载带 `idmap` 选项的 JuiceFS 卷，并通过 idmapped bind mount 验证 uid/gid 映射。
> 本机已完成全部步骤，内核升级（6.8 → 6.12+）后只需执行第 7 节的收尾验证。

## 0. 前提与网络

| 依赖 | 要求 |
|---|---|
| 内核 | **≥ 6.12**（FUSE_ALLOW_IDMAP）。Ubuntu 24.04 执行 `apt install linux-image-generic-hwe-24.04 && reboot`（当前源候选版本 7.0.0-42） |
| 软件 | docker、go ≥ 1.25、gcc；root 权限 |

受限网络下的替代源（本机已验证可用）：

```bash
export GOPROXY=https://goproxy.cn,direct                                   # Go 模块
docker pull docker.m.daocloud.io/<image>                                    # docker.io 镜像
docker pull k8s.m.daocloud.io/<registry.k8s.io 的镜像>                       # registry.k8s.io 镜像
https://gh-proxy.com/https://github.com/<...>                               # GitHub 下载（k3s 等）
```

## 1. 构建

所有产物在 `/root/code/idmap/verification/`，目录结构：

```
bin/       jfs(客户端) meta(meta server) k3s idmap_mount(.c 源码)
conf/      meta.cfg exports.cfg          # meta server 配置
conf.d/    vol1.conf                      # 客户端卷配置（挂载用）
image/     Dockerfile mount.juicefs(wrapper) Dockerfile.csi
manifests/ static-pv.yaml idmap-view-pod.yaml
logs/      运行日志
```

### 1.1 jfs 客户端与 meta server（jfs 仓库 idmap 分支）

```bash
cd /root/code/idmap/jfs
make -C cmd/meta meta          # -> cmd/meta/meta
make -C cmd/mount mount        # -> cmd/mount/mount（即 jfs 客户端，含全部子命令）
cp cmd/meta/meta  /root/code/idmap/verification/bin/meta
cp cmd/mount/mount /root/code/idmap/verification/bin/jfs
```

go.mod 已将 go-fuse 替换为 juicedata fork 的 `idmap-go-fuse`（分支 bdcc446+），`GOPROXY=goproxy.cn` 直接可拉。

### 1.2 idmapped mount 测试工具

Ubuntu 24.04 的 `mount` 不支持 `--map-users`，用小工具直接调三个系统调用：

```bash
gcc -O2 -o /root/code/idmap/verification/bin/idmap_mount \
    /root/code/idmap/verification/bin/idmap_mount.c
# 用法: idmap_mount <src> <dst> <ns_uid>:<host_uid>:<count>
```

**关键内核约束**（`fs/namespace.c can_idmap_mount()`）：`MOUNT_ATTR_IDMAP` 只能施加在**从未暴露给文件系统层级的匿名挂载**上，因此工具的流程是 `open_tree(OPEN_TREE_CLONE)` 克隆分离挂载 → fork 子进程建 user namespace 并由父进程写 uid_map/gid_map → `mount_setattr` 对分离挂载打上 IDMAP → `move_mount` 挂载到目标路径。直接对已 attach 的 bind mount 调 `mount_setattr` 会返回 EINVAL。

### 1.3 mount pod 镜像（juicefs-mount-idmap:dev）

关键点是 `image/mount.juicefs` wrapper：CSI 以 CE 方式调用 `/bin/mount.juicefs <metaurl> <path> -o idmap,metrics=...`，而 jfs 客户端是 EE 风格 CLI（`--idmap` 旗标、conf-dir 卷配置）。wrapper 做三件事：解析 `host:port/vol`、动态生成卷配置、把 `-o a,b=c` 翻译成 `--a --b=c`（丢弃 `metrics=`）。

```bash
cd /root/code/idmap/verification/image
cp ../bin/jfs .
docker build -t juicefs-mount-idmap:dev -f Dockerfile .
```

### 1.4 CSI 驱动补丁与镜像

CSI 挂载路径会强制执行 `juicefs config <metaurl>`（官方 CE 二进制解析我们的地址必失败）。补丁让 secret 里的 `uuid` 可以跳过该探测（`pkg/juicefs/juicefs.go`）：

```diff
-	jfsSetting, err := j.Settings(ctx, volumeID, uniqueId, "", secrets, volCtx, options)
+	// uuid from secret skips the `juicefs config` probe, needed for non-CE meta URLs
+	jfsSetting, err := j.Settings(ctx, volumeID, uniqueId, secrets["uuid"], secrets, volCtx, options)
```

```bash
cd /root/code/idmap/juicefs-csi-driver
GOPROXY=https://goproxy.cn,direct go build -o /root/code/idmap/verification/image/juicefs-csi-driver ./cmd
cd /root/code/idmap/verification/image
docker build -t juicefs-csi-driver-idmap:v0.33.2 -f Dockerfile.csi .   # 基于官方 v0.33.2 替换二进制
```

## 2. 本机 meta server + 冒烟挂载

```bash
# 配置已就位: conf/meta.cfg(端口 9431/9430) conf/exports.cfg(allow all) conf.d/vol1.conf(卷 vol1)
systemctl enable --now jfs-meta-idmap        # 服务文件 /etc/systemd/system/jfs-meta-idmap.service

# 冒烟挂载（--foreground --no-syslog 便于看日志）
mkdir -p /mnt/jfs-idmap-test
/root/code/idmap/verification/bin/jfs mount --conf-dir /root/code/idmap/verification/conf.d \
    vol1 /mnt/jfs-idmap-test --foreground --no-syslog --idmap
```

**判读**（`--idmap` 是否生效看这两行）：

- 内核 ≥ 6.12：`idmap: FUSE_ALLOW_IDMAP negotiated; kernel default_permissions and POSIX ACL checks enabled`
- 内核 < 6.12：`idmap: FUSE_ALLOW_IDMAP was not negotiated ...; falling back to an ordinary mount, idmapped bind mounts are unavailable`（挂载仍可用，只是不能做 idmapped bind mount）

卸载：`fusermount3 -u /mnt/jfs-idmap-test`（或 kill 进程）。

## 3. k3s 集群

```bash
# 二进制（走 gh-proxy）
curl -sL -o /usr/local/bin/k3s \
  "https://gh-proxy.com/https://github.com/k3s-io/k3s/releases/download/v1.31.2%2Bk3s1/k3s"
chmod +x /usr/local/bin/k3s && ln -sf /usr/local/bin/k3s /usr/local/bin/kubectl

# containerd 镜像源 /etc/rancher/k3s/registries.yaml：
#   docker.io -> https://docker.m.daocloud.io
#   registry.k8s.io -> https://k8s.m.daocloud.io

# systemd 服务 /etc/systemd/system/k3s.service：
#   ExecStart=/usr/local/bin/k3s server --disable traefik --disable servicelb \
#     --disable metrics-server --write-kubeconfig /root/.kube/config --write-kubeconfig-mode 600
systemctl enable --now k3s && kubectl get nodes    # Ready 即可

# 导入自建镜像
docker save juicefs-mount-idmap:dev | k3s ctr -n k8s.io images import -
docker save juicefs-csi-driver-idmap:v0.33.2 | k3s ctr -n k8s.io images import -
```

## 4. 部署 CSI 驱动

```bash
kubectl apply -f /root/code/idmap/juicefs-csi-driver/deploy/k8s.yaml

# 指定自建 mount 镜像 + 打过补丁的驱动
kubectl -n kube-system set env daemonset/juicefs-csi-node \
    JUICEFS_CE_MOUNT_IMAGE=juicefs-mount-idmap:dev JUICEFS_EE_MOUNT_IMAGE=juicefs-mount-idmap:dev
kubectl -n kube-system set env statefulset/juicefs-csi-controller \
    JUICEFS_CE_MOUNT_IMAGE=juicefs-mount-idmap:dev JUICEFS_EE_MOUNT_IMAGE=juicefs-mount-idmap:dev
kubectl -n kube-system set image daemonset/juicefs-csi-node juicefs-plugin=juicefs-csi-driver-idmap:v0.33.2
kubectl -n kube-system set image statefulset/juicefs-csi-controller juicefs-plugin=juicefs-csi-driver-idmap:v0.33.2
kubectl -n kube-system get pods    # controller 4/4、node 3/3、dashboard 1/1
```

## 5. 使用 idmap 卷（静态 PV）

`manifests/static-pv.yaml` 包含 4 个对象，要点：

- Secret：`metaurl: 172.30.117.100:9431/vol1`（**宿主机 IP**，pod 内 127.0.0.1 不通）+ `name: vol1`（必需）+ `uuid: vol1-uuid`（触发补丁跳过 config 探测）
- PV：`mountOptions: [idmap]`，通过 `nodePublishSecretRef` 引用 secret（kubelet 注入 NodePublishVolume）
- app pod：挂 PVC 到 /data 读写

```bash
kubectl apply -f /root/code/idmap/verification/manifests/static-pv.yaml
kubectl -n kube-system logs <mount-pod> | grep idmap     # 出现 negotiated/fallback 日志即证明 idmap 选项生效
```

mount pod 命令形如 `exec /bin/mount.juicefs ${metaurl} /jfs/xxx -o idmap,metrics=0.0.0.0:9567`（首行的 `juicefs format` 是 CSI 生成的历史遗留，失败不影响后续 exec）。

## 6. 验证 uid 映射（idmapped bind mount）

`manifests/idmap-view-pod.yaml`：privileged pod（挂 hostPath 的 idmap_mount 工具），对 PV 做 `idmap_mount /data /mnt/idmap 0:100000:65536`（pod uid 0 ↔ 文件系统 uid 100000）：

```bash
kubectl apply -f /root/code/idmap/verification/manifests/idmap-view-pod.yaml
kubectl logs jfs-idmap-view
```

**内核 ≥ 6.12 实测输出（内核 7.0.0-42，已验证）**：

```
idmapped mount created: /data -> /mnt/idmap (0:100000:65536)
=== MAP_OK: idmapped bind mount created ===
--- /mnt/idmap (idmapped view; expect view-100000.txt uid 100000) ---
-rw-r--r-- 1 101000 117183  3 ... a.txt
-rw-r--r-- 1 100000 100000 16 ... jfs-app.txt
-rw-r--r-- 1 100000 100000 16 ... view-100000.txt
--- /mnt/orig  (original view; expect view-100000.txt uid 0) ---
-rw-r--r-- 1 1000 17183    3 ... a.txt
-rw-r--r-- 1 0     0      16 ... jfs-app.txt
-rw-r--r-- 1 0     0      16 ... view-100000.txt
```

**映射语义**（映射 `0:100000:65536`，即 pod/userns uid N ↔ 文件系统 uid N+100000）：

| 文件系统真实 uid | 原始视图显示 | idmapped 视图显示 |
|---|---|---|
| 0（jfs-app.txt，root 创建） | 0 | 100000 |
| 1000 | 1000 | 101000 |
| pod 内以 uid 100000 经视图创建 | 0（落库为 0） | 100000 |

即容器场景下的经典语义：**userns 内的 root（=宿主 uid 100000）通过视图创建的文件以 fs uid 0 落库，且看到 fs uid 0 的文件属于自己的 root**。注意：映射范围外的 uid（如 init-ns 的 root 0）直接经视图创建文件会得到 EOVERFLOW，这是内核预期行为。

内核 < 6.12 预期：`mount_setattr(MOUNT_ATTR_IDMAP): Invalid argument` + `MAP_FAIL`。

## 7. 内核升级后的收尾验证（一键）

```bash
bash /root/code/idmap/verification/verify-after-reboot.sh
```

脚本做四件事：确认内核版本 → 重建 mount pod 并检查 `FUSE_ALLOW_IDMAP negotiated` 日志 → 跑 idmap-view pod 检查 MAP_OK 与双向 uid 翻译 → 汇总 pass/fail。

**实测结果（内核 7.0.0-42-generic）：10 pass / 0 fail。**

## 8. 清理

```bash
kubectl delete -f manifests/ && kubectl delete pod -n kube-system -l app.kubernetes.io/name=juicefs-mount --force
kubectl delete -f /root/code/idmap/juicefs-csi-driver/deploy/k8s.yaml
systemctl disable --now k3s jfs-meta-idmap
fusermount3 -u /mnt/jfs-idmap-test 2>/dev/null
```
