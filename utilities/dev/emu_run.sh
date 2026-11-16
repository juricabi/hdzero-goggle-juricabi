#!/bin/bash
# Sandboxed emulator run.
#   run.sh <build dir> <setting.ini|-> <key script> <shot prefix>
# Outer part (root, host namespaces): starts our own Xvfb on the first free display from :800,
# then re-executes itself in a private mount namespace (unshare -m) where:
#   /mnt is a fresh tmpfs (hides /mnt/c, /mnt/app); /mnt/app/language and /mnt/app/resource are
#   read-only bind mounts of the worktree's; no /mnt/app/script; /mnt/extsd is an empty dir
#   /tmp is a fresh tmpfs (the X socket dir bind-mounted back in)
#   the emulator binary, the stubs and the run dir live in that private /tmp, owned by 65534
# and the emulator runs as uid/gid 65534 with no groups, no capabilities (inheritable, ambient,
# bounding set) and no_new_privs, PATH starting with the logging stubs.
# Key script lines: "k <key> [n]" tap n times, "l <key>" hold 0.9 s, "w <s>", "s <name>" screenshot
set -u
E=${EMU_OUT:-/root/hdz/emu}
BUILD=$1 INI=$2 KEYS=$3 PREFIX=$4
SHOTS=$E/shots
LOGS=$E/logs
mkdir -p $SHOTS $LOGS

if [ -z "${IN_NS:-}" ]; then
  # our own display
  N=800
  while [ -e /tmp/.X$N-lock ] || [ -e /tmp/.X11-unix/X$N ]; do N=$((N + 1)); done
  Xvfb :$N -screen 0 1920x1080x24 -nolisten tcp -ac > /dev/null 2>&1 &
  XPID=$!
  sleep 1.5
  if ! kill -0 $XPID 2>/dev/null || [ "$(tr -d ' \n' < /tmp/.X$N-lock 2>/dev/null)" != "$XPID" ]; then
    echo "our Xvfb on :$N did not start; nothing run"
    kill $XPID 2>/dev/null
    exit 1
  fi
  unshare -m --propagation private env IN_NS=1 DISP=:$N XPID=$XPID bash "$0" "$@"
  rc=$?
  kill $XPID 2>/dev/null
  wait $XPID 2>/dev/null
  exit $rc
fi

# ---- inside the private mount namespace (still root here) ----
SRC=$(grep -E '^CMAKE_HOME_DIRECTORY' $BUILD/CMakeCache.txt | head -1 | sed 's/.*=//')
[ -d "$SRC/mkapp/app/language" ] || { echo "no source dir for $BUILD ($SRC)"; exit 1; }
mount -t tmpfs -o size=64m tmpfs /mnt
mkdir -p /mnt/.x11 /mnt/app/language /mnt/app/resource /mnt/extsd
mount --bind /tmp/.X11-unix /mnt/.x11
mount -t tmpfs -o size=128m tmpfs /tmp
mkdir -p /tmp/.X11-unix
mount --bind /mnt/.x11 /tmp/.X11-unix
mount --bind -o ro $SRC/mkapp/app/language /mnt/app/language
mount -o remount,bind,ro /mnt/app/language
mount --bind -o ro $SRC/mkapp/app/resource /mnt/app/resource
mount -o remount,bind,ro /mnt/app/resource
chown 65534:65534 /mnt/extsd

ST=/tmp/stubs
mkdir -p $ST
cat > $ST/_stub <<'EOF'
#!/bin/sh
echo "$(date +%T) $(basename "$0") $*" >> "${SANDBOX_LOG:-/dev/null}"
exit 0
EOF
cat > $ST/amixer <<'EOF'
#!/bin/sh
{ echo "### $(date +%T) amixer $*"; case " $* " in *" -s "*|*" --stdin "*) cat ;; esac; } >> "${AMIXER_LOG:-/dev/null}"
echo "$(date +%T) amixer $*" >> "${SANDBOX_LOG:-/dev/null}"
exit 0
EOF
chmod 755 $ST/_stub $ST/amixer
for c in passwd chpasswd usermod insmod rmmod modprobe killall pkill dropbear cp rm ln chmod chown mv dd \
         mount umount reboot poweroff halt shutdown ifconfig ip route hostapd udhcpd udhcpc wpa_supplicant \
         iw iwconfig aww awr dispw i2cset i2cget i2cdump gpiow gpior sync fsck mkfs.vfat mkfs.exfat \
         hwclock; do
  ln -sf _stub $ST/$c
done
RUN=/tmp/run
mkdir -p $RUN
cp $BUILD/HDZGOGGLE $RUN/
[ "$INI" != "-" ] && cp "$INI" $RUN/setting.ini
chown -R 65534:65534 $RUN
chmod 755 /tmp/stubs

cd $RUN
setpriv --reuid=65534 --regid=65534 --clear-groups --inh-caps=-all --bounding-set=-all --no-new-privs \
  env -i PATH=$ST:/usr/bin:/bin HOME=$RUN DISPLAY=$DISP SANDBOX_LOG=$RUN/stub.log AMIXER_LOG=$RUN/amixer.log \
  $RUN/HDZGOGGLE > $RUN/app.log 2>&1 &
EPID=$!
sleep 4
export DISPLAY=$DISP
WIN=$(xdotool search --onlyvisible --name . 2>/dev/null | head -1)
[ -n "$WIN" ] && xdotool windowfocus $WIN 2>/dev/null
xdotool mousemove 960 540
echo "uid of emulator: $(ps -o uid= -p $EPID 2>/dev/null) display $DISP" > $LOGS/${PREFIX}_run.txt
while read -r op arg n; do
  # still our Xvfb?
  kill -0 $XPID 2>/dev/null || { echo "our Xvfb is gone"; break; }
  kill -0 $EPID 2>/dev/null || { echo "emulator exited" >> $LOGS/${PREFIX}_run.txt; break; }
  case "$op" in
    k) for i in $(seq ${n:-1}); do xdotool keydown $arg; sleep 0.08; xdotool keyup $arg; sleep 0.3; done ;;
    l) xdotool keydown $arg; sleep 0.9; xdotool keyup $arg; sleep 0.5 ;;
    w) sleep $arg ;;
    s) sleep 0.6; import -window root $SHOTS/${PREFIX}_$arg.png ;;
    c) cp $RUN/setting.ini $LOGS/${PREFIX}_$arg.ini 2>/dev/null ;;
  esac
done < "$KEYS"
kill $EPID 2>/dev/null; sleep 0.3; kill -9 $EPID 2>/dev/null
wait $EPID 2>/dev/null
cp $RUN/app.log $LOGS/${PREFIX}_app.log 2>/dev/null
cp $RUN/stub.log $LOGS/${PREFIX}_stub.log 2>/dev/null
cp $RUN/amixer.log $LOGS/${PREFIX}_amixer.log 2>/dev/null
cp $RUN/setting.ini $LOGS/${PREFIX}_setting_after.ini 2>/dev/null
ls -la $RUN /tmp > $LOGS/${PREFIX}_tmp_listing.txt 2>&1
echo "done $PREFIX"
