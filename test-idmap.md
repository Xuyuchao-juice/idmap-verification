# JuiceFS idmap 基础功能验证计划

## 一、现状结论（调研结果）

| 项 | 结论 |
|---|---|
| 本机内核 | 6.8（不满足 6.12+），待你升级到 HWE 7.0.0-42；`/root/code/linux`（7.1 源码树，已含 FUSE_ALLOW_IDMAP）可作为备用方案 |
| go-fuse 依赖 | juicedata/go-fuse 有配套分支 **`idmap-go-fuse`**（bdcc446b），jfs 的 go.mod 已指向 `../go-fuse`，clone 到位即可 |
| jfs CLI | EE 风格：`jfs mount host:port/vol /mp`（内置 file 存储、无需 format 步骤）+ `meta` server 二进制（cmd/meta，需 `-c` 配置文件，细节执行时确认） |
| CSI 驱动 | **无 idmap 相关代码**，但 mountOptions 从 StorageClass → mount pod 完全透传、无白名单（`builder/common.go:174-219` 拼装 `-o ...,idmap`），基础验证可零改动，只需自建 mount pod 镜像并通过 `JUICEFS_EE_MOUNT_IMAGE` 环境变量注入 |
| 工具 | util-linux 2.39.3 原生支持 `mount --map-users`（idmapped bind mount），无需额外工具 |

## 二、阶段 0：环境准备

1. **内核升级（你操作）**：`apt install linux-image-generic-hwe-24.04 && reboot`。⚠️ 重启会中断本会话，恢复后我继续；构建产物都在磁盘不受影响。
2. go-fuse 就位：`git clone juicedata/go-fuse /root/code/idmap/go-fuse && checkout idmap-go-fuse`
3. 构建：`make -C cmd/mount`、`make -C cmd/meta`（或直接 `go build`）；`go build ./...` 确认编译通过
4. 单测：`go test ./pkg/... ./cmd/...`（重点 pkg/meta、pkg/vfs、pkg/fuse）
5. （CSI 阶段前）安装 kind + kubectl，创建单节点集群

## 三、阶段 2：idmapped mount 核心行为（裸机，1 天）

拓扑：本机 `meta` server + `jfs mount 127.0.0.1:PORT/vol /mnt/jfs --foreground --idmap`（挂载日志确认 `FUSE_ALLOW_IDMAP negotiated`）。

建立映射视图：`mount --bind /mnt/jfs /mnt/jfs-idmap && mount --map-users=100000:0:65536 ...`，宿主 uid 100000 ↔ 视图 uid 0。

| 编号 | 验证点 | 预期 |
|---|---|---|
| V2.1 | **uid 双向翻译**：宿主侧建 uid=100000 文件→视图显示 root；视图内 root 建文件→meta 落库 uid=100000（创建类请求带映射后 uid，非创建类带哨兵 -1） | stat 结果按映射互译 |
| V2.2 | 非 root 映射（视图 1000 ↔ 宿主 101000） | 同上 |
| V2.3 | chown/chmod 翻译与属主检查 | 视图内改自己文件 OK；改他人文件 EPERM |
| V2.4 | **内核 DAC 强制**：无权限访问 | 内核直接 EACCES；到达 juicefs 的请求 uid=-1（oplog 验证） |
| V2.5 | sticky bit（1777 目录） | 非 owner 删他人文件被拒（内核检查），owner 正常 |
| V2.6 | sgid 继承 | sgid 目录下建文件继承 sgid（服务端跳过 inGroup 检查） |
| V2.7 | xattr 规则 | `system.*` 写→ENOTSUP；`security.capability`（加 `--enable-cap`）→允许；其他 `security.*`→拒；`trusted.*`→视图内 root 可读写 |
| V2.8 | ACL | setfacl→ENOTSUP；但内核按已有 POSIX ACL 做权限判定 |
| V2.9 | ioctl | `chattr +i`→EOPNOTSUPP；`lsattr`→OK |
| V2.10 | `.control` | rmr/compact/clone/seteattr/appendfile→EOPNOTSUPP |
| V2.11 | oplog | 哨兵请求显示 `uid:-1` |
| V2.12 | 对照组 | 同操作在**普通挂载**（无 idmap）上行为不变，确认服务端 DAC 只对 idmapped 会话跳过 |
| V2.13 | （可选）mapall+idmap 共存 | 挂载成功、default_permissions 强制开启、meta 仍按 mapalluid 检查 |

## 四、阶段 3：CSI/K8s 验证（1 天）

1. 自建镜像（jfs + meta 二进制，按 EE mount 镜像布局：`/usr/local/bin/juicefs` + `/sbin/mount.juicefs` 软链），`kind load` 进集群
2. 部署官方 `deploy/k8s.yaml`（v0.33.2 零改动），node DS 的 `juicefs-plugin` 容器加 `JUICEFS_EE_MOUNT_IMAGE` 指向自建镜像；meta server 以 Deployment+Service 部署，file 存储走 hostPath
3. Secret（metaurl）→ SC（`mountOptions: ["idmap"]`）→ PVC → app pod

| 编号 | 验证点 | 预期 |
|---|---|---|
| V3.1 | PVC bound，mount pod 正常 | mount pod 日志含 FUSE_ALLOW_IDMAP negotiated |
| V3.2 | 普通 app pod 读写 | 正常，uid 不变（存量行为兼容） |
| V3.3 | **核心场景**：privileged pod（CAP_SYS_ADMIN）内对 PV 做 `mount --bind --map-users` 并以映射 uid 读写 | 与 V2.1 断言一致（k8s 尚不支持自动 idmap CSI 卷，由 pod 内自建映射视图模拟容器场景） |
| V3.4 | idmap 视图 pod 与普通视图 pod 共享同一卷 | 互不干扰 |
| V3.5 | 回归：不带 idmap 的 SC | 正常挂载 |

## 五、产出物

`/root/code/idmap/verification/`：可重复执行的脚本（setup/断言/清理）+ `RESULT.md`（每验证点 pass/fail + 证据日志片段）。

## 六、风险与待确认

- **R1** EE mount 镜像里 `/sbin/mount.juicefs` wrapper 对 `-o idmap` 的解析（可能不认识该选项，需要小改 wrapper 或 CSI 命令拼装——执行时确认，必要时给 CSI 提最小补丁）
- **R2** `.control` 触发 CLI 在本仓库缺失（cmd 只有 mount/meta），V2.10 可能需手写 `.control` 报文或降级为代码级单测
- **R3** meta server 配置文件格式需执行时摸索（exports 定义 volume）
- **R4** mapall 为 EE exports 功能、配置较重，V2.13 标可选

顺序建议：内核升级完成后，从阶段 0 开始，依次执行阶段 2、阶段 3。你 review 通过、内核升级完成后告诉我，我开始实操。
