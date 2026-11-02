// idmap_mount: create an idmapped bind mount via open_tree/mount_setattr/move_mount.
// usage: idmap_mount <src> <dst> <ns_uid>:<host_uid>:<count>
// Must run as root (CAP_SYS_ADMIN) in the mount's owning user namespace.
// The kernel only allows MOUNT_ATTR_IDMAP on a detached (anonymous) mount,
// so the source is cloned with open_tree(OPEN_TREE_CLONE), idmapped, then
// attached at dst with move_mount().
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/mount.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MOUNT_ATTR_IDMAP
#define MOUNT_ATTR_IDMAP 0x00100000
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif
#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif

static void die(const char *msg) { perror(msg); exit(1); }

static int send_fd(int sock, int fd) {
    char b = 0;
    struct iovec iov = {.iov_base = &b, .iov_len = 1};
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *(int *)CMSG_DATA(cmsg) = fd;
    return sendmsg(sock, &msg, 0);
}

static int recv_fd(int sock) {
    char b;
    struct iovec iov = {.iov_base = &b, .iov_len = 1};
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    if (recvmsg(sock, &msg, 0) < 0) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;
    return *(int *)CMSG_DATA(cmsg);
}

static void write_file(const char *path, const char *content) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) die(path);
    if (write(fd, content, strlen(content)) < 0) die(path);
    close(fd);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <src> <dst> <nsuid>:<hostuid>:<count>\n", argv[0]);
        return 2;
    }
    const char *src = argv[1], *dst = argv[2];

    int nsuid, hostuid, count;
    if (sscanf(argv[3], "%d:%d:%d", &nsuid, &hostuid, &count) != 3) {
        fprintf(stderr, "bad map spec %s\n", argv[3]);
        return 2;
    }

    // 1. clone the source into a detached mount (anonymous mount namespace)
    int tfd = syscall(SYS_open_tree, AT_FDCWD, src,
                      OPEN_TREE_CLONE | AT_RECURSIVE | O_CLOEXEC);
    if (tfd < 0) die("open_tree(OPEN_TREE_CLONE)");

    // 2. fork: child creates a userns; we (still privileged) write its id maps
    int sk[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sk) < 0) die("socketpair");
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        close(sk[0]);
        if (unshare(CLONE_NEWUSER) < 0) die("child unshare(CLONE_NEWUSER)");
        char b = 1;
        if (write(sk[1], &b, 1) < 0) die("child sync write");
        int ufd = open("/proc/self/ns/user", O_RDONLY);
        if (ufd < 0) die("child open ns/user");
        if (send_fd(sk[1], ufd) < 0) die("child send userns fd");
        pause(); // stay alive: the idmapped mount must keep a live userns
        _exit(0);
    }
    close(sk[1]);
    char b;
    if (read(sk[0], &b, 1) != 1) die("parent sync read");

    char map[128], pmap[128];
    snprintf(map, sizeof(map), "%d %d %d", nsuid, hostuid, count);
    snprintf(pmap, sizeof(pmap), "/proc/%d/uid_map", pid);
    write_file(pmap, map);
    snprintf(pmap, sizeof(pmap), "/proc/%d/setgroups", pid);
    write_file(pmap, "deny");
    snprintf(pmap, sizeof(pmap), "/proc/%d/gid_map", pid);
    write_file(pmap, map);

    int ufd = recv_fd(sk[0]);
    if (ufd < 0) die("parent recv userns fd");

    // 3. apply the idmapping to the detached mount
    struct mount_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.attr_set = MOUNT_ATTR_IDMAP;
    attr.userns_fd = ufd;
    if (syscall(SYS_mount_setattr, tfd, "", AT_RECURSIVE | AT_EMPTY_PATH,
                &attr, sizeof(attr)) < 0)
        die("mount_setattr(MOUNT_ATTR_IDMAP)");
    close(ufd);

    // 4. attach the idmapped mount at dst
    if (mkdir(dst, 0755) < 0 && errno != EEXIST) die(dst);
    if (syscall(SYS_move_mount, tfd, "", AT_FDCWD, dst,
                MOVE_MOUNT_F_EMPTY_PATH) < 0)
        die("move_mount");
    close(tfd);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    printf("idmapped mount created: %s -> %s (%s)\n", src, dst, argv[3]);
    return 0;
}
