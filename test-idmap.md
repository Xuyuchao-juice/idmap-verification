# JuiceFS idmap 基础功能验证计划

## 一、现状结论（调研结果）

| 项 | 结论 |
|---|---|
| 用户真实用法 | Kubernetes `hostUsers: false`（User Namespaces，v1.33 起默认开启，v1.36 GA）。容器运行时自动为 Pod 的**每个卷**创建 idmapped mount；卷的文件系统不支持时 Pod 启动失败（`failed to set MOUNT_ATTR_IDMAP ... invalid argument`）。JuiceFS idmap 的目标就是让 JuiceFS PVC 能被这类 Pod 使用，用户不会自己执行 `mount --map-users` |
| 内核 | FUSE 自 6.12 支持 idmapped mount，且 FUSE server 必须声明 `FUSE_ALLOW_IDMAP` 并开启 `default_permissions`，否则挂载失败（`fs/fuse/inode.c`）。Ubuntu 24.04 HWE 内核 7.0.0-42 满足 |
| 容器运行时 | 需 containerd ≥ 2.0（或 CRI-O ≥ 1.25）+ runc ≥ 1.2（或 crun ≥ 1.9）。上一轮的 k3s v1.31.2 内置 containerd 1.7.22 + runc 1.1.14，**不满足**；改用 k3s v1.37.1+k3s1（containerd 2.3.4、runc 1.4.2） |
| go-fuse 依赖 | juicedata/go-fuse 有配套分支 **`idmap-go-fuse`**（bdcc446b），jfs 的 go.mod 已指向 `../go-fuse`，clone 到位即可 |
| jfs CLI | EE 风格：`jfs mount host:port/vol /mp`（内置 file 存储、无需 format 步骤）+ `meta` server 二进制（cmd/meta，配置见 `conf/meta.cfg`、`conf/exports.cfg`） |
| CSI 驱动 | 无 idmap 相关代码，mountOptions 从 PV/StorageClass 透传到 mount pod（`builder/common.go:174-219` 拼装 `-o ...,idmap`）。但 CE 挂载路径会强制执行 `juicefs config` 探测，需要 CSI 补丁（secret `uuid` 跳过探测）+ mount 镜像 wrapper 转换参数，见 QUICKSTART 1.3/1.4 |
| 工具 | Ubuntu 24.04 的 util-linux 2.39.3 **不支持** `mount --map-users`（2.40 起），但支持 `-o X-mount.idmap=`。仓库的 `idmap_mount` 与 runc 的系统调用顺序一致，仅用于低层排障 |
| 上一轮结论 | 只证明了 FUSE 协商成功、内核允许在 JuiceFS 上建 idmapped mount、基础 uid 翻译方向正确。访问者是初始命名空间里无 capability 的 uid 100000，**未覆盖容器 root 的特权语义，不能作为用户场景验收** |

## 二、阶段 0：环境准备

1. **内核 ≥ 6.12**：`apt install linux-image-generic-hwe-24.04 && reboot`，重启后 `uname -r` 确认。
2. go-fuse 就位：`git clone juicedata/go-fuse /root/code/idmap/go-fuse && checkout idmap-go-fuse`
3. 构建：`make -C cmd/mount`、`make -C cmd/meta`（或直接 `go build`）；`go build ./...` 确认编译通过
4. 单测：`go test ./pkg/... ./cmd/...`（重点 pkg/meta、pkg/vfs、pkg/fuse）
5. （K8s 阶段前）安装 k3s v1.37.1+k3s1（不再使用 kind 或 k3s v1.31.2），确认 containerd ≥ 2.0、runc ≥ 1.2，且 `hostUsers: false` 冒烟 Pod 能运行（QUICKSTART 第 3 节、6.1 节）

## 三、阶段 1：裸机低层验证（前置条件）

拓扑：本机 `meta` server + `jfs mount ... --idmap`（QUICKSTART 第 2 节）。本阶段只确认 FUSE 层能力和服务端行为，**不验收用户语义**；容器 root 的权限语义统一放到阶段 2 的 user namespace Pod 中验证。

| 编号 | 验证点 | 预期 |
|---|---|---|
| V1.1 | 挂载协商 | 日志含 `FUSE_ALLOW_IDMAP negotiated` |
| V1.2 | 低层映射挂载：`idmap_mount /mnt/jfs-idmap-test /mnt/jfs-view 0:100000:65536` | 成功；文件系统 uid N 在视图中显示为 N+100000 |
| V1.3 | 对照：不带 `--idmap` 挂载后执行 V1.2 | `mount_setattr(MOUNT_ATTR_IDMAP): Invalid argument` |
| V1.4 | oplog | 经 idmapped mount 的非创建类请求 uid 为哨兵 `-1`，创建类请求带映射后的 uid |
| V1.5 | `.control` | rmr/compact/clone/seteattr/appendfile→EOPNOTSUPP |
| V1.6 | 对照组 | 同操作在**普通挂载**（无 idmap）上行为不变，确认服务端 DAC 只对 idmapped 会话跳过 |
| V1.7 | （可选）mapall+idmap 共存 | 挂载成功、default_permissions 强制开启、meta 仍按 mapalluid 检查 |

V1.1–V1.3 通过后再进入阶段 2。

## 四、阶段 2：K8s 用户场景（主验收）

拓扑：k3s v1.37.1 + 打补丁的 CSI + 自建 mount 镜像；静态 PV `mountOptions: [idmap]`。`jfs-app`（普通 Pod）作为磁盘原始 uid 的参照视图，`jfs-userns`（`hostUsers: false`）为被测 Pod。操作步骤和命令见 QUICKSTART 第 6 节。

验收语义：容器内看到和写入的 uid/gid 与磁盘上存储的一致；权限判定与普通容器 root 在本地文件系统上的行为一致。

| 编号 | 验证点 | 预期 |
|---|---|---|
| U0 | 运行时冒烟：`hostUsers: false`，不挂外部卷 | Running，uid_map 为 `0 <X> 65536` |
| U1 | idmap PV 被 userns Pod 挂载 | Running；mount pod 日志含 `FUSE_ALLOW_IDMAP negotiated` |
| U2 | 读路径翻译 | 容器内 uid/gid 与参照视图一致；> 65535 的 id 显示 65534 |
| U3 | 创建路径 | 容器 root 创建 → 落盘 0；容器 uid 1000 创建 → 落盘 1000 |
| U4–U5 | 容器 root 的 DAC override | 属主在映射内可越权读；属主在映射外 Permission denied |
| U6 | 普通用户 DAC | 无权限时 Permission denied |
| U7–U8 | chown/chmod | root 可 chown 到映射内 id，映射外 Invalid argument；非属主 chown/chmod EPERM；root chmod 他人文件成功 |
| U9 | sticky 目录 | 非属主删除 EPERM，root 删除成功 |
| U10 | sgid 目录 | 文件继承属组、子目录继承 setgid 位；非组成员 chmod g+s 被清除 |
| U11 | ACL | setfacl→ENOTSUP；getfacl 正常；内核仍按已有 POSIX ACL 判权 |
| U12 | xattr | `user.*` 正常；`trusted.*` EPERM（内核拦截）；`system.*` 写 ENOTSUP |
| U13 | ioctl | `chattr +i` EPERM（内核拦截） |
| U14 | `security.capability` | 记录是否带 `--enable-cap` 时的实际结果 |
| U15 | 共享卷 | userns Pod 与普通 Pod 同时读写同一卷，双方可见对方写入，属主一致 |
| U16 | 存量兼容 | 普通 Pod（`jfs-app`）使用带 idmap 的 PV，读写与 uid 不受影响 |
| U17 | **负向对照**：不带 idmap 的 PV 给 userns Pod | Pod 启动失败，事件含 `MOUNT_ATTR_IDMAP ... invalid argument` |

## 五、产出物

`/root/code/idmap/verification/`：manifests（含 `userns-pod.yaml`、`plain-pv.yaml`）、验证脚本 + `RESULT.md`（每个验证点 pass/fail + 证据：命令输出、Pod 事件、mount pod 日志片段）。

## 六、风险与待确认

- **R1** `image/mount.juicefs` wrapper 和 CSI `uuid` 补丁是测试用适配；产品化需要 EE mount 镜像原生支持 `-o idmap`
- **R2** `.control` 触发 CLI 在本仓库缺失（cmd 只有 mount/meta），V1.5 可能需手写 `.control` 报文或降级为代码级单测
- **R3** juicefs-csi-driver v0.33.2 与 Kubernetes 1.37 的兼容性未验证；PV 挂载异常时先看 csi-node 日志
- **R4** U12–U14 同时受内核与 JuiceFS 限制，错误码以实测为准并记录；内核先拦截时无法观察到 JuiceFS 侧限制，这部分靠 V1.x 与代码级单测覆盖
- **R5** 用户数据中 uid/gid > 65535 的文件，在 userns Pod 内显示为 65534 且不可修改（K8s 默认每个 Pod 65536 个 ID，可用 kubelet `userNamespaces.idsPerPod` 扩大），需要写入用户文档
- **R6** mapall 为 EE exports 功能、配置较重，V1.7 标可选

顺序建议：阶段 0 → 阶段 1（V1.1–V1.3）→ 阶段 2（U0 不通过时不要继续）→ 汇总 `RESULT.md`。
