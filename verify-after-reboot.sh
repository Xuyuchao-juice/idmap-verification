#!/bin/bash
# Post-kernel-upgrade verification for JuiceFS idmap (CSI path).
# Run as root after rebooting into kernel >= 6.12.
set -u
cd "$(dirname "$0")"

pass=0; fail=0
check() { # name, condition(0=ok)
    if [ "$2" -eq 0 ]; then echo "[PASS] $1"; pass=$((pass+1)); else echo "[FAIL] $1"; fail=$((fail+1)); fi
}

echo "=== 1. kernel version ==="
KVER=$(uname -r)
echo "kernel: $KVER"
minor=$(echo "$KVER" | sed -E 's/^([0-9]+)\.([0-9]+).*/\2/')
major=$(echo "$KVER" | sed -E 's/^([0-9]+)\..*/\1/')
[ "$major" -gt 6 ] || { [ "$major" -eq 6 ] && [ "$minor" -ge 12 ]; }
check "kernel >= 6.12" $?

echo "=== 2. services up ==="
systemctl is-active --quiet k3s;            check "k3s active" $?
systemctl is-active --quiet jfs-meta-idmap; check "meta server active" $?
sleep 15
kubectl -n kube-system get pods --no-headers 2>/dev/null | grep -qE "juicefs-csi-node.*3/3.*Running"
check "csi node plugin running" $?

echo "=== 3. recreate mount pod and check negotiation ==="
kubectl delete pod jfs-app jfs-idmap-view --force --grace-period=0 2>/dev/null
kubectl -n kube-system delete pod -l app.kubernetes.io/name=juicefs-mount --force --grace-period=0 2>/dev/null
kubectl apply -f manifests/static-pv.yaml >/dev/null 2>&1
sleep 45
MPOD=$(kubectl -n kube-system get pods -l app.kubernetes.io/name=juicefs-mount -o custom-columns=N:.metadata.name --no-headers | grep jfs-idmap-pv | head -1)
echo "mount pod: $MPOD"
if [ -n "$MPOD" ]; then
    kubectl -n kube-system logs "$MPOD" 2>/dev/null | grep -q "FUSE_ALLOW_IDMAP negotiated"
    check "mount pod log: FUSE_ALLOW_IDMAP negotiated" $?
else
    check "mount pod exists" 1
fi
kubectl get pod jfs-app >/dev/null 2>&1 && [ "$(kubectl get pod jfs-app -o jsonpath='{.status.phase}')" = "Running" ]
check "app pod running" $?

echo "=== 4. idmapped bind mount + uid translation ==="
kubectl delete pod jfs-idmap-view --force --grace-period=0 2>/dev/null
kubectl apply -f manifests/idmap-view-pod.yaml >/dev/null 2>&1
OUT=""
for i in $(seq 1 12); do  # wait until the pod prints both listings
    sleep 5
    OUT=$(kubectl logs jfs-idmap-view 2>/dev/null)
    echo "$OUT" | grep -A2 -- "--- /mnt/orig" | grep -q "^total" && break
done
echo "$OUT" | grep -q "MAP_OK"
check "idmapped bind mount created (MAP_OK)" $?
echo "$OUT" | grep -q "idmapped mount created"
check "idmap_mount tool succeeded" $?
# view-100000.txt (created as pod uid 100000): uid 100000 in mapped view, uid 0 in original view
echo "$OUT" | grep -A5 -- "--- /mnt/idmap" | grep "view-100000.txt" | grep -Eq "[[:space:]]100000[[:space:]]+100000[[:space:]]"
check "idmapped view shows view-100000.txt as uid 100000" $?
echo "$OUT" | grep -A5 -- "--- /mnt/orig" | grep "view-100000.txt" | grep -Eq "[[:space:]]0[[:space:]]+0[[:space:]]"
check "original view shows view-100000.txt as uid 0" $?

echo ""
echo "=== summary: $pass pass, $fail fail ==="
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
