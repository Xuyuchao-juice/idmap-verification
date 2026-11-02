# JuiceFS idmap + CSI 快速搭建与验证教程

> 目标：在一台 Ubuntu 24.04 机器上，从零搭建 k3s + juicefs-csi-driver 环境，验证 **JuiceFS 卷能被 `hostUsers: false`（user namespace）Pod 正常使用**。这是用户的真实用法：容器运行时会自动为 Pod 的每个卷创建 idmapped mount，卷的文件系统不支持时 Pod 直接启动失败。
>
> 验证分两层：
> - **主验收（第 6 节）**：`hostUsers: false` Pod 挂载 JuiceFS PVC，由 containerd/runc 自动创建 idmapped mount，并以容器内 root（拥有 user namespace 内的 capability）执行读写与特权操作。
> - **低层排障（第 7 节）**：用 `idmap_mount` 手动建映射视图，只用于定位内核/FUSE 层问题，不能代替主验收。
>
> 上一轮使用 k3s v1.31.2（内置 containerd 1.7.22 + runc 1.1.14），不支持 user namespace Pod 的卷 idmap，只完成了低层验证。本文按新环境要求从头执行。

## 0. 前提与网络

| 依赖 | 要求 |
|---|---|
| 内核 | **≥ 6.12**（FUSE_ALLOW_IDMAP）。Ubuntu 24.04 执行 `apt install linux-image-generic-hwe-24.04 && reboot`（当前源候选版本 7.0.0-42） |
| Kubernetes | **≥ 1.33**（User Namespaces 默认开启，1.36 GA）。本文使用 k3s v1.37.1+k3s1 |
| 容器运行时 | **containerd ≥ 2.0 + runc ≥ 1.2**（k3s v1.37.1+k3s1 内置 containerd 2.3.4、runc 1.4.2） |
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
manifests/ static-pv.yaml userns-pod.yaml plain-pv.yaml idmap-view-pod.yaml
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

### 1.2 idmapped mount 测试工具（仅用于第 7 节低层排障）

Ubuntu 24.04 自带 util-linux 2.39.3：没有 `mount --map-users/--map-groups`（2.40 起才有），但已支持 `-o X-mount.idmap=`。仓库仍保留一个直接调用系统调用的小工具，因为它与 runc 为容器卷创建 idmapped mount 的调用顺序一致，出错时能直接看出是哪一步失败：

```bash
gcc -O2 -o /root/code/idmap/verification/bin/idmap_mount /root/code/idmap/verification/bin/idmap_mount.c
# 用法: idmap_mount <src> <dst> <ns_uid>:<host_uid>:<count>
```

按 mount(8) 的语义，等价的 util-linux 写法为 `mount --bind -o X-mount.idmap=b:100000:0:65536 <src> <dst>`（文件系统 uid 0 在新挂载中显示为 100000），**尚未实测**。

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
/root/code/idmap/verification/bin/jfs mount --conf-dir /root/code/idmap/verification/conf.d vol1 /mnt/jfs-idmap-test --foreground --no-syslog --idmap
```

**判读**（`--idmap` 是否生效看这两行）：

- 内核 ≥ 6.12：`idmap: FUSE_ALLOW_IDMAP negotiated; kernel default_permissions and POSIX ACL checks enabled`
- 内核 < 6.12：`idmap: FUSE_ALLOW_IDMAP was not negotiated ...; falling back to an ordinary mount, idmapped bind mounts are unavailable`（挂载仍可用，只是不能做 idmapped bind mount）

卸载：`fusermount3 -u /mnt/jfs-idmap-test`（或 kill 进程）。

## 3. k3s 集群

> 必须使用内置 containerd ≥ 2.0、runc ≥ 1.2 的版本。旧的 v1.31.2+k3s1（containerd 1.7.22 + runc 1.1.14）无法运行第 6 节的主验收。

```bash
# 二进制（走 gh-proxy）
curl -sL -o /usr/local/bin/k3s "https://gh-proxy.com/https://github.com/k3s-io/k3s/releases/download/v1.37.1%2Bk3s1/k3s"
chmod +x /usr/local/bin/k3s && ln -sf /usr/local/bin/k3s /usr/local/bin/kubectl

# containerd 镜像源 /etc/rancher/k3s/registries.yaml：
#   docker.io -> https://docker.m.daocloud.io
#   registry.k8s.io -> https://k8s.m.daocloud.io

# systemd 服务 /etc/systemd/system/k3s.service：
#   ExecStart=/usr/local/bin/k3s server --disable traefik --disable servicelb --disable metrics-server --write-kubeconfig /root/.kube/config --write-kubeconfig-mode 600
systemctl enable --now k3s && kubectl get nodes    # Ready 即可

# 确认版本满足 user namespace 卷 idmap 的要求
k3s --version                                            # Kubernetes ≥ 1.33
k3s ctr version                                          # containerd ≥ 2.0
/var/lib/rancher/k3s/data/current/bin/runc --version     # runc ≥ 1.2

# 导入自建镜像
docker save juicefs-mount-idmap:dev | k3s ctr -n k8s.io images import -
docker save juicefs-csi-driver-idmap:v0.33.2 | k3s ctr -n k8s.io images import -
```

## 4. 部署 CSI 驱动

```bash
kubectl apply -f /root/code/idmap/juicefs-csi-driver/deploy/k8s.yaml

# 指定自建 mount 镜像 + 打过补丁的驱动
kubectl -n kube-system set env daemonset/juicefs-csi-node JUICEFS_CE_MOUNT_IMAGE=juicefs-mount-idmap:dev JUICEFS_EE_MOUNT_IMAGE=juicefs-mount-idmap:dev
kubectl -n kube-system set env statefulset/juicefs-csi-controller JUICEFS_CE_MOUNT_IMAGE=juicefs-mount-idmap:dev JUICEFS_EE_MOUNT_IMAGE=juicefs-mount-idmap:dev
kubectl -n kube-system set image daemonset/juicefs-csi-node juicefs-plugin=juicefs-csi-driver-idmap:v0.33.2
kubectl -n kube-system set image statefulset/juicefs-csi-controller juicefs-plugin=juicefs-csi-driver-idmap:v0.33.2
kubectl -n kube-system get pods    # controller 4/4、node 3/3、dashboard 1/1
```

> juicefs-csi-driver v0.33.2 与 Kubernetes 1.37 的兼容性尚未验证。若 PV 挂载异常，先查看 `juicefs-csi-node` 的 `juicefs-plugin` 容器日志。

## 5. 使用 idmap 卷（静态 PV）

`manifests/static-pv.yaml` 包含 4 个对象，要点：

- Secret：`metaurl: 172.30.117.100:9431/vol1`（**宿主机 IP**，pod 内 127.0.0.1 不通）+ `name: vol1`（必需）+ `uuid: vol1-uuid`（触发补丁跳过 config 探测）
- PV：`mountOptions: [idmap]`，通过 `nodePublishSecretRef` 引用 secret（kubelet 注入 NodePublishVolume）
- app pod（`jfs-app`）：挂 PVC 到 /data 读写。它**不启用 user namespace**，第 6 节将其作为“磁盘原始 uid”的参照视图

```bash
kubectl apply -f /root/code/idmap/verification/manifests/static-pv.yaml
kubectl -n kube-system logs <mount-pod> | grep idmap     # 出现 negotiated/fallback 日志即证明 idmap 选项生效
```

mount pod 命令形如 `exec /bin/mount.juicefs ${metaurl} /jfs/xxx -o idmap,metrics=0.0.0.0:9567`（首行的 `juicefs format` 是 CSI 生成的历史遗留，失败不影响后续 exec）。

## 6. 主验收：user namespace Pod 使用 idmap 卷

用户侧只需在 Pod 上设置 `hostUsers: false`。kubelet 为 Pod 分配一段宿主 uid（`0 → X`，X 为 65536 的倍数），containerd/runc 再对 Pod 的**每个卷**执行 `open_tree → mount_setattr(MOUNT_ATTR_IDMAP) → move_mount`，映射与 Pod 的 user namespace 相同。

验收语义（Kubernetes 文档约定）：**容器内看到和写入的 uid/gid，与磁盘上存储的一致**，与是否启用 user namespace 无关；0–65535 之外的 uid/gid 在容器内显示为 65534，且不可修改。

### 6.1 运行时冒烟（不涉及 JuiceFS）

```bash
kubectl run userns-smoke --image=ubuntu:24.04 --restart=Never --overrides='{"spec":{"hostUsers":false}}' -- cat /proc/self/uid_map
kubectl logs userns-smoke && kubectl delete pod userns-smoke    # Pod Completed 后执行
```

预期输出 `0 <X> 65536`，X ≥ 65536。这一步不通过时，先排查第 3 节的运行时版本，不要继续往下测。

### 6.2 部署 user namespace Pod

`manifests/userns-pod.yaml` 中的 `jfs-userns` 与 `jfs-app` 使用同一个 PVC（`jfs-idmap-pvc`）。启动时会安装 acl、attr、libcap2-bin、e2fsprogs（软件源替换为 mirrors.aliyun.com，按网络环境调整）。

```bash
kubectl apply -f /root/code/idmap/verification/manifests/userns-pod.yaml
kubectl logs -f jfs-userns    # 出现 READY 后 Ctrl-C
ps -eo uid,pid,args | grep 'sleep infinity' | grep -v grep    # 宿主上执行：jfs-app 为 0，jfs-userns 为 X
```

### 6.3 准备测试文件（在参照视图 jfs-app 中执行）

```bash
kubectl exec jfs-app -- sh -c 'rm -rf /data/u && mkdir -p /data/u/sticky /data/u/sgid && cd /data/u && echo a > f1000 && chown 1000:1000 f1000 && chmod 600 f1000 && echo b > f2000 && chown 2000:2000 f2000 && chmod 644 f2000 && echo c > f70000 && chown 70000:70000 f70000 && chmod 600 f70000 && chmod 1777 sticky && echo d > sticky/f2000 && chown 2000:2000 sticky/f2000 && chown 0:3000 sgid && chmod 2777 sgid && ls -lnR /data/u'
```

### 6.4 用例

约定：

- 未注明时，命令在 `jfs-userns` 内以容器 root 执行：`kubectl exec jfs-userns -- sh -c '<命令>'`。
- “uid 1000：”表示以 `setpriv --reuid 1000 --regid 1000 --clear-groups <命令>` 执行。
- “参照视图”表示 `kubectl exec jfs-app -- ls -ln /data/u`。
- 按编号顺序执行，后面的用例依赖前面创建的文件。

| 编号 | 操作 | 预期 | 验证点 |
|---|---|---|---|
| U1 | `id -u; cat /proc/self/uid_map` | `0`；`0 <X> 65536` | Pod 运行在 user namespace 中 |
| U2 | `ls -ln /data/u` | f1000、f2000、sgid 的属主/属组与参照视图一致；f70000 显示为 `65534 65534` | 读路径 uid/gid 翻译、映射范围外的 id |
| U3 | `touch /data/u/by-root`；uid 1000：`touch /data/u/by-1000`；再看参照视图 | by-root 为 `0 0`，by-1000 为 `1000 1000` | 创建路径：落盘 uid = 容器内 uid |
| U4 | `cat /data/u/f1000` | 成功，输出 `a` | 容器 root 的 CAP_DAC_OVERRIDE 在 user namespace 内生效 |
| U5 | `cat /data/u/f70000` | Permission denied | 属主不在映射范围时 capability 不生效 |
| U6 | uid 1000：`cat /data/u/f2000`；uid 1000：`sh -c "echo x >> /data/u/f2000"` | 读成功；写 Permission denied | 普通用户 DAC |
| U7 | `chown 2000:2000 /data/u/by-root`；`chown 70000 /data/u/by-root` | 前者成功，参照视图为 `2000 2000`；后者 Invalid argument | CAP_CHOWN、不可映射的 id |
| U8 | uid 1000：`chown 1000 /data/u/f2000`；uid 1000：`chmod 666 /data/u/f2000`；`chmod 640 /data/u/f2000` | 前两条 Operation not permitted；最后一条成功 | 属主检查、CAP_FOWNER |
| U9 | uid 1000：`rm -f /data/u/sticky/f2000`；随后 `rm -f /data/u/sticky/f2000` | 前者 Operation not permitted；后者成功 | sticky 目录 |
| U10 | uid 1000：`sh -c "touch /data/u/sgid/x; mkdir /data/u/sgid/d; touch /data/u/sgid/y; chmod 2755 /data/u/sgid/y"`；`ls -ln /data/u/sgid` | x、d 的属组为 3000，d 带 setgid 位；y 的 setgid 位被清除（`-rwxr-xr-x`） | SGID 继承与清除 |
| U11 | `setfacl -m u:1000:r /data/u/by-1000`；`getfacl /data/u/by-1000` | 前者 Operation not supported；后者正常输出 | idmap 会话禁止修改 ACL |
| U12 | `setfattr -n user.k -v v /data/u/by-1000 && getfattr -n user.k /data/u/by-1000`；`setfattr -n trusted.k -v v /data/u/by-1000` | user.* 成功；trusted.* Operation not permitted（内核要求初始命名空间的 CAP_SYS_ADMIN） | xattr |
| U13 | `chattr +i /data/u/by-1000` | Operation not permitted（内核要求初始命名空间的 CAP_LINUX_IMMUTABLE） | ioctl |
| U14 | `cp /bin/true /data/u/t && setcap cap_net_raw+ep /data/u/t && getcap /data/u/t` | 记录实际结果；与 mount pod 是否带 `--enable-cap` 有关 | security.capability |

### 6.5 负向对照：不带 idmap 的卷

```bash
kubectl apply -f /root/code/idmap/verification/manifests/plain-pv.yaml
kubectl describe pod jfs-userns-plain    # Events 中应出现 MOUNT_ATTR_IDMAP ... invalid argument
kubectl -n kube-system get pods -l app.kubernetes.io/name=juicefs-mount    # jfs-plain-pv 应有独立的 mount pod，其日志中没有 FUSE_ALLOW_IDMAP negotiated
```

预期 `jfs-userns-plain` 无法进入 Running。这证明 user namespace Pod 能使用 JuiceFS 卷，正是 `idmap` 挂载选项带来的。

## 7. 低层排障：手动 idmapped mount 视图（可选）

`manifests/idmap-view-pod.yaml`：privileged pod（挂 hostPath 的 idmap_mount 工具），对 PV 做 `idmap_mount /data /mnt/idmap 0:100000:65536`（文件系统 uid N 在映射视图中显示为 N+100000），再用 `setpriv` 切换到 uid 100000 经视图写文件。

> 局限：写文件的进程是初始命名空间里**没有任何 capability** 的 uid 100000，而不是 user namespace 内的 root；映射挂载也由测试者手动创建，而非容器运行时。因此本节只能证明内核允许在 JuiceFS 上建立映射挂载、基础 uid 翻译方向正确，**不能作为用户场景的验收结果**。

```bash
kubectl apply -f /root/code/idmap/verification/manifests/idmap-view-pod.yaml
kubectl logs jfs-idmap-view
```

**上一轮实测输出（内核 7.0.0-42，k3s v1.31.2）**：

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

**映射语义**（映射 `0:100000:65536`：文件系统 uid N ↔ 映射视图 uid N+100000）：

| 文件系统真实 uid | 原始视图显示 | idmapped 视图显示 |
|---|---|---|
| 0（jfs-app.txt，root 创建） | 0 | 100000 |
| 1000 | 1000 | 101000 |
| pod 内以 uid 100000 经视图创建 | 0（落库为 0） | 100000 |

即容器场景下的经典语义：**userns 内的 root（=宿主 uid 100000）通过视图创建的文件以 fs uid 0 落库，且看到 fs uid 0 的文件属于自己的 root**。注意：映射范围外的 uid（如 init-ns 的 root 0）直接经视图创建文件会得到 EOVERFLOW，这是内核预期行为。

内核 < 6.12 预期：`mount_setattr(MOUNT_ATTR_IDMAP): Invalid argument` + `MAP_FAIL`。

## 8. 低层路径一键验证（脚本）

```bash
bash /root/code/idmap/verification/verify-after-reboot.sh
```

脚本做四件事：确认内核版本 → 重建 mount pod 并检查 `FUSE_ALLOW_IDMAP negotiated` 日志 → 跑 idmap-view pod 检查 MAP_OK 与双向 uid 翻译 → 汇总 pass/fail。

> 脚本只覆盖低层路径（第 2、5、7 节），**不包含第 6 节主验收**。上一轮在 k3s v1.31.2 上的结果为 10 pass / 0 fail，仅代表低层路径。

## 9. 清理

```bash
kubectl delete -f /root/code/idmap/verification/manifests/ && kubectl delete pod -n kube-system -l app.kubernetes.io/name=juicefs-mount --force
kubectl delete -f /root/code/idmap/juicefs-csi-driver/deploy/k8s.yaml
systemctl disable --now k3s jfs-meta-idmap
fusermount3 -u /mnt/jfs-idmap-test 2>/dev/null
```
