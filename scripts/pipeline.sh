#!/usr/bin/env bash
# Local release pipeline: build, check and package shclaw on every platform
# we test, from one git revision, then write a report.
#
#   scripts/pipeline.sh [--rev REV] [--only t1,t2] [--live] [--list]
#
#   --rev   git revision to build (default: HEAD); uncommitted changes are ignored
#   --only  comma-separated targets (default: all, see --list)
#   --live  also run the live smoke test (needs an OpenAI key, see LIVE_DIR)
#
# Host specifics come from scripts/pipeline.conf (copy pipeline.conf.example).
# Output: <OUT>/<version>/ with the archives, SHA256SUMS, report.md and logs/.
# Targets run one after the other: one VM at a time, never a Docker build
# while a VM runs. The laptop ran out of memory once, and i386 guests stall
# the host with split-locked atomics, so qemu runs niced, i386 guests on 1 CPU.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD

TARGETS="linux-x86_64 cosmo-x86_64 linux-i386 linux-armv7l linux-aarch64
openbsd-amd64 openbsd-i386 freebsd-amd64 freebsd-i386 netbsd-amd64 netbsd-i386
cosmo-on-freebsd freebsd-i386-on-amd64 cosmo-on-netbsd"
# The last three ship nothing: they run binaries built by other targets

REV=HEAD ONLY="" LIVE=0
while [ $# -gt 0 ]; do
    case $1 in
        --rev) REV=$2; shift ;;
        --only) ONLY=$2; shift ;;
        --live) LIVE=1 ;;
        --list) echo $TARGETS; exit 0 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

[ -f scripts/pipeline.conf ] || { echo "missing scripts/pipeline.conf (see pipeline.conf.example)" >&2; exit 2; }
. scripts/pipeline.conf
if [ $LIVE = 1 ] && [ ! -f "$LIVE_DIR/etc/config.ini" ]; then
    echo "--live needs $LIVE_DIR/etc/config.ini" >&2; exit 2
fi

COMMIT=$(git rev-parse --verify "$REV^{commit}") || exit 2
VERSION=$(git show "$COMMIT:include/tc.h" | sed -n 's/.*TC_VERSION *"\(.*\)".*/\1/p')
[ -n "$VERSION" ] || { echo "no TC_VERSION at $REV" >&2; exit 2; }
OUTV=$OUT/$VERSION
WORK=$OUTV/work
BUNDLE=$WORK/bundle.tar
[ -z "$ONLY" ] && rm -rf "${OUTV:?}"
mkdir -p "$OUTV/logs" "$WORK"

# Stop any VM left running (interrupt, error)
cleanup() {
    for f in "$VM_DIR"/vm/*.pipeline.pid; do
        [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null
        rm -f "$f"
    done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

archive() { echo "shclaw-$VERSION-$1.tar.gz"; }

# Source at REV plus the vendored sources (from a cache checked by vendor.sh),
# so no target downloads anything
make_bundle() {
    local key cache b=$WORK/bundle
    key=$(git show "$COMMIT:vendor.sh" | sha256sum | cut -c1-12)
    cache=$OUT/vendor-cache/$key
    if [ ! -f "$cache/vendor/cjson/cJSON.c" ]; then
        rm -rf "$cache" && mkdir -p "$cache/vendor"
        git show "$COMMIT:vendor.sh" > "$cache/vendor.sh"
        # Seed the two slow clones from this checkout: vendor.sh checks their pins
        for v in bearssl tcc; do
            [ -d "$ROOT/vendor/$v/.git" ] && git clone -q "$ROOT/vendor/$v" "$cache/vendor/$v"
        done
        if ! (cd "$cache" && sh vendor.sh); then
            rm -rf "$cache/vendor" && mkdir -p "$cache/vendor"
            (cd "$cache" && sh vendor.sh) || return 1
        fi
    fi
    rm -rf "$b" && mkdir -p "$b" && git archive "$COMMIT" | tar -xf - -C "$b" &&
        (cd "$cache" && tar --exclude=.git -cf - vendor/bearssl vendor/tcc vendor/cjson) | tar -xf - -C "$b" &&
        tar -C "$b" -cf "$BUNDLE" . &&
        cp "$b/include/tc_plugin.h" "$WORK/tc_plugin.h" &&
        rm -rf "$b"
}

fresh() { rm -rf "$1" && mkdir -p "$1" && tar -xf "$BUNDLE" -C "$1"; }

collect() {   # collect <archive>: into the release dir
    [ -s "$1" ] || { echo "missing $1"; return 1; }
    cp "$1" "$OUTV/" && echo "==> $(basename "$1") ($(stat -c %s "$1") bytes)"
}

# Command runners: one shell command string each, stdin passed through;
# TMO=<seconds> in front of a call bounds it
local_sh() { timeout "${TMO:-7200}" sh -c "$1"; }
pi_sh() { timeout "${TMO:-7200}" ssh -o BatchMode=yes -o ConnectTimeout=10 "$PI_HOST" "$1"; }
vm_sh() {
    timeout "${TMO:-7200}" ssh -q -i "$VM_DIR/vmkey" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o BatchMode=yes \
        -p "$VM_PORT" root@127.0.0.1 "$1"
}

# live <runner> <archive on the target> <work dir on the target>
live() {
    [ $LIVE = 1 ] || return 0
    local run=$1 arch=$2 dir=$3 inst rc
    inst=$dir/$(basename "$arch" .tar.gz)
    $run "rm -rf $dir && mkdir -p $dir && tar -xzf $arch -C $dir" &&
        tar -C "$LIVE_DIR" -cf - etc/agents plugins/weather_city.c plugins/currency_exchange.c |
            $run "tar -xf - -C $inst" &&
        $run "umask 077; cat > $inst/etc/config.ini" < "$LIVE_DIR/etc/config.ini" &&
        $run "cat > $dir/live.sh" < scripts/pipeline-live.sh || return 1
    TMO=1800 $run "sh $dir/live.sh $inst"
    rc=$?
    $run "rm -f $inst/etc/config.ini"
    return $rc
}

# ── Linux, on this machine ──────────────────────────────────────────────

t_linux_x86_64() {
    local d=$WORK/linux-x86_64
    fresh "$d" && (cd "$d" && make musl && make check && make dist) &&
        collect "$d/dist/$(archive linux-x86_64)" &&
        live local_sh "$OUTV/$(archive linux-x86_64)" "$d/live"
}

t_cosmo_x86_64() {
    local d=$WORK/cosmo-x86_64 link
    fresh "$d" && ln -s "$COSMO_DIR" "$d/vendor/cosmo" &&
        (cd "$d" && make cosmo && make check-cosmo && make dist) &&
        collect "$d/dist/$(archive cosmo-x86_64)" || return 1
    # The checks as a binary the BSDs run: linked as .com, then assimilate -b
    link=$(cd "$d" && make -n check-cosmo | grep -- '-o build-cosmo/check ' | sed 's|-o build-cosmo/check |-o check.com |')
    (cd "$d" && eval "$link" && vendor/cosmo/bin/assimilate -b check.com) &&
        cp "$d/check.com" "$WORK/cosmo-check.com" &&
        live local_sh "$OUTV/$(archive cosmo-x86_64)" "$d/live"
}

t_linux_i386() {   # Docker: everything built for the i586, see the script
    local d=$WORK/linux-i386
    rm -rf "$d" && scripts/release-linux-i386.sh "$BUNDLE" "$d/out" &&
        collect "$d/out/$(archive linux-i386)" &&
        live local_sh "$OUTV/$(archive linux-i386)" "$d/live"
}

# ── ARM board ───────────────────────────────────────────────────────────

t_linux_armv7l() {
    local d=$PI_DIR/armv7l a
    a=$(archive linux-armv7l)
    # linux32: the armhf userland also under a 64-bit kernel (TinyCC's
    # configure reads uname -m); ARCH only names the archive
    pi_sh "rm -rf $d && mkdir -p $d && tar -xf - -C $d" < "$BUNDLE" &&
        pi_sh "cd $d && linux32 make musl && linux32 make check && make dist ARCH=armv7l" &&
        pi_sh "cat $d/dist/$a" > "$WORK/$a" && collect "$WORK/$a" &&
        live pi_sh "$d/dist/$a" "$PI_DIR/live-armv7l"
}

t_linux_aarch64() {   # in the Alpine aarch64 chroot (sudo on the board)
    local r=$PI_A64_ROOT a
    a=$(archive linux-aarch64)
    pi_sh "sudo rm -rf $r/pipeline && sudo mkdir $r/pipeline && sudo tar -xf - -C $r/pipeline" < "$BUNDLE" &&
        pi_sh "mountpoint -q $r/proc || sudo mount -t proc proc $r/proc
            mountpoint -q $r/dev || sudo mount --bind /dev $r/dev
            sudo chroot $r /bin/sh -c 'command -v gcc >/dev/null || apk add --no-cache build-base >/dev/null
                cd /pipeline && make musl MUSL_CC=gcc && make check MUSL_CC=gcc && make dist MUSL_CC=gcc'
            rc=\$?; sudo umount $r/dev $r/proc; exit \$rc" &&
        pi_sh "cat $r/pipeline/dist/$a" > "$WORK/$a" && collect "$WORK/$a" &&
        live pi_sh "$r/pipeline/dist/$a" "$PI_DIR/live-aarch64"
}

# ── BSD virtual machines ────────────────────────────────────────────────

VM_PORT="" VM_PID=""

vm_start() {   # vm_start <name>: boot it (see VMS in pipeline.conf), wait for SSH
    local line free i=0
    line=$(printf '%s\n' "$VMS" | awk -v n="$1" '$1 == n')
    [ -n "$line" ] || { echo "no VM $1 in pipeline.conf"; return 1; }
    if pgrep -f qemu-system >/dev/null; then echo "another VM is running"; return 1; fi
    free=$(free -g | awk '/^Mem:/ {print $7}')
    [ "$free" -ge "$VM_MIN_FREE_GB" ] || { echo "only ${free}G available"; return 1; }
    set -- $line   # name qemu MB CPUs disk bus NIC port
    VM_PORT=$8 VM_PID=$VM_DIR/vm/$1.pipeline.pid
    nice -n 19 ionice -c3 "$2" -enable-kvm -cpu host -m "$3" -smp "$4" \
        -drive "file=$VM_DIR/vm/$5,if=$6,format=qcow2" \
        -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$8-:22" -device "$7,netdev=n0" \
        -display none -vga std -daemonize -pidfile "$VM_PID" || return 1
    until TMO=30 vm_sh true 2>/dev/null; do
        i=$((i + 1))
        [ $i -lt 60 ] || { echo "no SSH on port $VM_PORT"; return 1; }
        sleep 5
    done
    echo "booted $(TMO=30 vm_sh 'uname -srm')"
}

vm_stop() {
    local pid i=0
    [ -f "$VM_PID" ] || return 0
    pid=$(cat "$VM_PID")
    TMO=30 vm_sh "/sbin/shutdown -p now" >/dev/null 2>&1
    while kill -0 "$pid" 2>/dev/null && [ $i -lt 40 ]; do sleep 3; i=$((i + 1)); done
    kill "$pid" 2>/dev/null
    rm -f "$VM_PID"
}

vm_native() {   # vm_native <target>: gmake native, check-native and dist in its VM
    local t=$1 d=/root/pipeline/$1 a rc=0
    a=$(archive "$t")
    vm_start "$t" &&
        vm_sh "rm -rf $d && mkdir -p $d && tar -xf - -C $d" < "$BUNDLE" &&
        vm_sh "cd $d && PATH=/usr/local/bin:/usr/pkg/bin:\$PATH && export PATH &&
               gmake native && gmake check-native && gmake dist" &&
        vm_sh "cat $d/dist/$a" > "$WORK/$a" && collect "$WORK/$a" || rc=1
    if [ $rc = 0 ] && [ "$t" = freebsd-i386 ]; then   # for freebsd-i386-on-amd64
        vm_sh "cat $d/build/check" > "$WORK/freebsd-i386-check" || rc=1
    fi
    if [ $rc = 0 ]; then live vm_sh "$d/dist/$a" /root/pipeline/live || rc=1; fi
    vm_stop
    return $rc
}

vm_run() {   # vm_run <vm> <check binary> <archive>: another target's binaries on this VM
    local vm=$1 bin=$2 a=$3 d=/root/pipeline/run rc=0
    if [ ! -s "$bin" ] || [ ! -s "$OUTV/$a" ]; then
        echo "SKIP: needs $(basename "$bin") and $a from this run"; return 3
    fi
    vm_start "$vm" &&
        vm_sh "rm -rf $d && mkdir -p $d/include && cat > $d/include/tc_plugin.h" < "$WORK/tc_plugin.h" &&
        vm_sh "cat > $d/check && chmod +x $d/check" < "$bin" &&
        vm_sh "cd $d && ./check" &&
        vm_sh "cat > $d/$a" < "$OUTV/$a" &&
        live vm_sh "$d/$a" "$d/live" || rc=1
    vm_stop
    return $rc
}

t_openbsd_amd64() { vm_native openbsd-amd64; }
t_openbsd_i386() { vm_native openbsd-i386; }
t_freebsd_amd64() { vm_native freebsd-amd64; }
t_freebsd_i386() { vm_native freebsd-i386; }
t_netbsd_amd64() { vm_native netbsd-amd64; }
t_netbsd_i386() { vm_native netbsd-i386; }
t_cosmo_on_freebsd() { vm_run freebsd-amd64 "$WORK/cosmo-check.com" "$(archive cosmo-x86_64)"; }
t_freebsd_i386_on_amd64() { vm_run freebsd-amd64 "$WORK/freebsd-i386-check" "$(archive freebsd-i386)"; }
t_cosmo_on_netbsd() { vm_run netbsd-amd64 "$WORK/cosmo-check.com" "$(archive cosmo-x86_64)"; }

# ── Run and report ──────────────────────────────────────────────────────

fmt() { printf '%dm%02ds' $(($1 / 60)) $(($1 % 60)); }

details() {   # the lines of a target log that matter, joined
    grep -E '==> Built|All checks passed|check\(s\) failed|^FAIL|==> shclaw-|^LIVE|^booted|^SKIP|no SSH|available$|another VM|^missing' "$1" |
        sed 's/^==> //' | cut -c1-110 | awk '!seen[$0]++' | paste -sd'~' | sed 's/~/ · /g'
}

PASSED=0 FAILED=0 SKIPPED=0 ROWS=""
run_target() {
    local t=$1 log=$OUTV/logs/$1.log start rc status dur extra=""
    start=$(date +%s)
    printf '==> %-22s ' "$t"
    ("t_${t//-/_}") > "$log" 2>&1
    rc=$?
    dur=$(fmt $(($(date +%s) - start)))
    case $rc in
        0) status="✅ pass"; PASSED=$((PASSED + 1)) ;;
        3) status="⏭️ skip"; SKIPPED=$((SKIPPED + 1)) ;;
        *) status="❌ FAIL"; FAILED=$((FAILED + 1))
           extra=$(grep -iE 'error|fail|not found' "$log" | grep -v '^LIVE' | tail -1 | cut -c1-110) ;;
    esac
    echo "$status ($dur)"
    ROWS+="| $t | $status | $dur | $(details "$log")${extra:+ · $extra} |"$'\n'
}

echo "shclaw $VERSION from $(git log -1 --format='%h %s' "$COMMIT") into $OUTV"
printf '==> %-22s ' bundle
if make_bundle > "$OUTV/logs/bundle.log" 2>&1; then echo ok; else echo "FAIL (see logs/bundle.log)"; exit 1; fi

for t in $TARGETS; do
    if [ -n "$ONLY" ] && [[ ",$ONLY," != *",$t,"* ]]; then continue; fi
    run_target "$t"
done

(cd "$OUTV" && ls shclaw-*.tar.gz >/dev/null 2>&1 && sha256sum shclaw-*.tar.gz > SHA256SUMS)
{
    echo "# shclaw $VERSION: pipeline report"
    echo
    echo "- Revision: \`$(git rev-parse --short "$COMMIT")\` $(git log -1 --format=%s "$COMMIT")"
    echo "- Date: $(date '+%Y-%m-%d %H:%M')${ONLY:+, targets: $ONLY}"
    echo "- Live smoke test: $([ $LIVE = 1 ] && echo yes || echo no)"
    echo "- Result: $PASSED passed, $FAILED failed, $SKIPPED skipped"
    echo
    echo "| Target | Result | Time | Details |"
    echo "|--------|--------|------|---------|"
    printf '%s' "$ROWS"
} > "$OUTV/report.md"
echo "report: $OUTV/report.md"
[ $FAILED = 0 ]
