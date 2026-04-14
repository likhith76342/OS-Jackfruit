# Step-by-Step Run Guide
# Multi-Container Runtime — From Files to Full Demo

==============================================================
ASSUMED STARTING POINT:
  You have these files in one folder on your Ubuntu VM:
    engine.c
    monitor.c
    monitor_ioctl.h
==============================================================

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 0 — ONE-TIME SYSTEM SETUP  (do this once per VM)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Install build tools and kernel headers
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) wget

# Verify headers exist (must not be empty)
ls /lib/modules/$(uname -r)/build

# Expected output: something like
# /lib/modules/6.x.x-generic/build -> /usr/src/linux-headers-6.x.x-generic

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 1 — PROJECT FOLDER SETUP
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Create your project directory and enter it
mkdir -p ~/container-runtime
cd ~/container-runtime

# Copy your three source files here
# (If they're on your Desktop or Downloads, adjust path)
cp ~/Downloads/engine.c        .
cp ~/Downloads/monitor.c       .
cp ~/Downloads/monitor_ioctl.h .

# Verify all three are present
ls -la
# Expected:
#   engine.c
#   monitor.c
#   monitor_ioctl.h

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 2 — CREATE THE MAKEFILE
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Create the Makefile (copy-paste this entire block exactly)
cat > Makefile << 'MAKEFILE_EOF'
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Wno-stringop-truncation
LDFLAGS = -lpthread

KDIR   ?= /lib/modules/$(shell uname -r)/build
PWD    := $(shell pwd)

obj-m += monitor.o

.PHONY: all clean ci module engine

all: engine module

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c $(LDFLAGS)

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

ci: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine_ci engine.c $(LDFLAGS)

clean:
	rm -f engine engine_ci
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
MAKEFILE_EOF

# IMPORTANT: The lines starting with $(CC) and $(MAKE) must use a TAB
# character, not spaces. The cat heredoc above preserves this correctly.
# If you manually edit the Makefile, press Tab (not spaces) before those lines.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 3 — BUILD EVERYTHING
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Build user-space binary + kernel module in one command
make all

# You should see something like:
#   gcc -Wall -Wextra -O2 ... -o engine engine.c -lpthread
#   make -C /lib/modules/6.x.x/build M=/home/.../container-runtime modules
#   CC [M] /home/.../container-runtime/monitor.o
#   MODPOST /home/.../container-runtime/Module.symvers
#   LD [M] /home/.../container-runtime/monitor.ko

# Verify outputs
ls -la engine monitor.ko
# engine      <- user-space binary
# monitor.ko  <- kernel module

# If module build fails with "No rule to make target", check:
uname -r                                       # your kernel version
ls /lib/modules/$(uname -r)/build              # headers must exist here
sudo apt install linux-headers-$(uname -r)     # install if missing

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 4 — PREPARE THE ALPINE ROOT FILESYSTEM
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Download Alpine mini rootfs (only do this once)
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz

# Create base rootfs directory and extract into it
mkdir -p rootfs-base
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Verify it extracted correctly
ls rootfs-base/
# Expected: bin dev etc home lib media mnt opt proc root run sbin srv sys tmp usr var

# Create the log output directory
mkdir -p logs

# Create per-container writable copies
# Each running container MUST have its own separate rootfs copy
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

# Verify copies exist
ls -d rootfs-*/
# Expected: rootfs-alpha/  rootfs-base/  rootfs-beta/

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 5 — LOAD THE KERNEL MODULE
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Load the module into the kernel (requires sudo)
sudo insmod monitor.ko

# Verify it loaded successfully — check for the device file
ls -la /dev/container_monitor
# Expected: crw-rw-rw- 1 root root 235, 0 ... /dev/container_monitor

# Also verify via dmesg
dmesg | tail -3
# Expected line:
#   [container_monitor] Module loaded. Device: /dev/container_monitor

# If you see "Operation not permitted":
#   -> Secure Boot is ON. Go to VM settings and disable it, then reboot.

# If you see "Key was rejected by service":
#   -> Same issue. Secure Boot must be OFF in your VM settings.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 6 — OPEN TERMINALS
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

You need 3 terminal windows all in the same directory.
Open them now:

  Terminal 1 = Supervisor  (stays running the whole time)
  Terminal 2 = CLI commands (you type commands here)
  Terminal 3 = Monitoring  (watch logs and kernel messages)

In EVERY terminal, navigate to your project folder:
  cd ~/container-runtime

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 7 — START THE SUPERVISOR  [Terminal 1]
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# In Terminal 1:
sudo ./engine supervisor ./rootfs-base

# Expected output:
#   [supervisor] Started. rootfs=./rootfs-base socket=/tmp/mini_runtime.sock

# The supervisor now SITS HERE and waits.
# Do NOT press Ctrl+C — leave this running.
# All further commands go in Terminal 2.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 8 — START CONTAINERS  [Terminal 2]
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Start container "alpha" using rootfs-alpha
# Runs /bin/sh with soft limit 48 MiB, hard limit 80 MiB
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80

# Expected response:
#   started 'alpha' pid=<some number>

# Start container "beta" using rootfs-beta
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 32 --hard-mib 64

# Expected response:
#   started 'beta' pid=<some number>

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 9 — LIST CONTAINERS (ps)  [Terminal 2]
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

sudo ./engine ps

# Expected output (columns: ID, PID, STATE, SOFT MiB, HARD MiB, REASON):
#   ID               PID      STATE      SOFT(MiB)  HARD(MiB)  REASON
#   alpha            1234     running    48         80         none
#   beta             1235     running    32         64         none

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 10 — VIEW LOGS  [Terminal 2]
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# View what container alpha has printed to stdout/stderr
sudo ./engine logs alpha

# The log file is also directly readable:
cat logs/alpha.log
cat logs/beta.log

# Watch logs update in real time (Terminal 3):
tail -f logs/alpha.log

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 11 — WATCH KERNEL MONITOR EVENTS  [Terminal 3]
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# In Terminal 3, watch kernel messages in real time:
sudo dmesg --follow | grep container_monitor

# You will see lines like:
#   [container_monitor] Registered container=alpha pid=1234 soft=50331648 hard=83886080
#   [container_monitor] Registered container=beta  pid=1235 soft=33554432 hard=67108864

# When a container hits the soft limit:
#   [container_monitor] SOFT LIMIT container=alpha pid=1234 rss=51234567 limit=50331648

# When a container hits the hard limit (killed):
#   [container_monitor] HARD LIMIT container=alpha pid=1234 rss=85000000 limit=83886080

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 12 — TEST MEMORY LIMITS
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# First, build a memory workload program
cat > mem_workload.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char *argv[]) {
    int target = (argc > 1) ? atoi(argv[1]) : 100;
    int total  = 0, step = 5;
    printf("Allocating up to %d MiB...\n", target);
    fflush(stdout);
    while (total < target) {
        char *p = malloc(step * 1024 * 1024);
        if (!p) { printf("malloc failed at %d MiB\n", total); break; }
        memset(p, 1, step * 1024 * 1024);
        total += step;
        printf("Allocated %d MiB\n", total);
        fflush(stdout);
        sleep(2);
    }
    printf("Holding %d MiB. Sleeping...\n", total);
    fflush(stdout);
    sleep(120);
    return 0;
}
EOF

gcc -O2 -o mem_workload mem_workload.c

# Copy the workload binary into the container's rootfs
# (must be inside rootfs so chroot can find it)
cp mem_workload ./rootfs-alpha/mem_workload

# Now start a container with tight limits to trigger soft + hard limit
sudo ./engine start memtest ./rootfs-alpha /mem_workload --soft-mib 20 --hard-mib 35

# Watch Terminal 3 (dmesg) — you'll see:
#   SOFT LIMIT warning at ~20 MiB
#   HARD LIMIT kill    at ~35 MiB

# After the kill, check ps to see state changed:
sudo ./engine ps
# memtest should now show state=killed reason=hard_limit_killed

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 13 — TEST THE run COMMAND (blocks until exit)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# First make a short workload that exits on its own
cat > short_task.sh << 'EOF'
#!/bin/sh
echo "Task started"
sleep 5
echo "Task done"
EOF
cp short_task.sh ./rootfs-alpha/short_task.sh
chmod +x ./rootfs-alpha/short_task.sh

# run blocks your terminal until the container exits
sudo ./engine run gamma ./rootfs-alpha /short_task.sh

# Expected (waits ~5 seconds, then prints):
#   running 'gamma' pid=XXXX (waiting...)
#   container 'gamma' exited code=0 signal=0 reason=exited

# After it returns, check ps:
sudo ./engine ps
# gamma should show state=exited

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 14 — STOP A RUNNING CONTAINER
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Stop container alpha gracefully
sudo ./engine stop alpha

# Expected response:
#   stopped 'alpha'

# Verify state changed
sudo ./engine ps
# alpha should now show state=stopped reason=stopped

# Stop beta too
sudo ./engine stop beta

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 15 — SCHEDULING EXPERIMENT (Task 5)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Build CPU workload
cat > cpu_workload.c << 'EOF'
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
int main(int argc, char *argv[]) {
    int dur = (argc > 1) ? atoi(argv[1]) : 30;
    time_t start = time(NULL);
    volatile long x = 0;
    while (time(NULL) - start < dur) x++;
    printf("done: %ld iters in %d seconds\n", x, dur);
    return 0;
}
EOF

gcc -O2 -o cpu_workload cpu_workload.c

# Copy into both container rootfs directories
cp cpu_workload ./rootfs-alpha/cpu_workload
cp cpu_workload ./rootfs-beta/cpu_workload

# If you stopped the containers above, restart fresh copies:
cp -a rootfs-base rootfs-exp1
cp -a rootfs-base rootfs-exp2
cp cpu_workload ./rootfs-exp1/cpu_workload
cp cpu_workload ./rootfs-exp2/cpu_workload

# Experiment: same CPU work, different nice values
# nice=0 gets more CPU time than nice=10
sudo ./engine start exp1 ./rootfs-exp1 /cpu_workload --nice 0
sudo ./engine start exp2 ./rootfs-exp2 /cpu_workload --nice 10

# In Terminal 3, watch CPU share every 3 seconds:
watch -n 3 'ps -o pid,ni,%cpu,cmd ax | grep cpu_workload | grep -v grep'

# Expected: exp1 (nice=0) uses ~3x more CPU than exp2 (nice=10)
# Record the %CPU column numbers for your report.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 16 — CLEAN SHUTDOWN
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Step 1: Stop all running containers first
sudo ./engine stop exp1
sudo ./engine stop exp2
# Stop any others that are still running

# Step 2: Confirm no containers are running
sudo ./engine ps
# All should show stopped/exited/killed

# Step 3: Shut down the supervisor
# In Terminal 1, press:  Ctrl+C
# You should see:
#   [supervisor] Shutting down...
#   [supervisor] Clean exit.

# Step 4: Verify no zombie processes remain
ps aux | grep defunct
# Should return NOTHING (or just the grep itself)

# Step 5: Verify no engine processes remain
ps aux | grep "engine supervisor"
# Should return NOTHING

# Step 6: Verify socket file is cleaned up
ls /tmp/mini_runtime.sock
# Should say: No such file or directory

# Step 7: Unload the kernel module
sudo rmmod monitor

# Step 8: Confirm module unloaded
dmesg | tail -3
# Expected:
#   [container_monitor] Module unloaded.

# Step 9: Confirm device file is gone
ls /dev/container_monitor
# Should say: No such file or directory

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PHASE 17 — IF YOU NEED TO RE-RUN FROM SCRATCH
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Clean build artifacts
make clean

# Remove old rootfs copies (keep rootfs-base)
rm -rf rootfs-alpha rootfs-beta rootfs-exp1 rootfs-exp2

# Remove log files
rm -rf logs/

# Remove stale socket if supervisor crashed without cleanup
rm -f /tmp/mini_runtime.sock

# Rebuild from scratch
make all

# Recreate rootfs copies
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

# Reload module
sudo insmod monitor.ko

# Now go back to Phase 7

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
TROUBLESHOOTING
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

PROBLEM: make all fails with "No rule to make target"
FIX:     sudo apt install linux-headers-$(uname -r)

PROBLEM: sudo insmod monitor.ko → "Operation not permitted"
FIX:     Secure Boot is ON. Disable it in VM settings → restart VM.

PROBLEM: sudo insmod monitor.ko → "Invalid module format"
FIX:     Module was built for a different kernel. Run:
         make clean && make all
         (Always rebuild after a reboot or kernel update)

PROBLEM: engine start → "connect (is supervisor running?)"
FIX:     Supervisor is not running. Go to Terminal 1 and run:
         sudo ./engine supervisor ./rootfs-base

PROBLEM: engine start → "container 'alpha' already exists"
FIX:     A container with that ID is still registered.
         Either stop it: sudo ./engine stop alpha
         Or use a different ID: sudo ./engine start alpha2 ...

PROBLEM: clone() fails with permission denied
FIX:     Run with sudo. clone() with namespace flags needs root.

PROBLEM: /dev/container_monitor does not exist
FIX:     Module is not loaded. Run: sudo insmod monitor.ko

PROBLEM: dmesg shows nothing from container_monitor
FIX:     Check module is loaded: lsmod | grep monitor
         If not listed: sudo insmod monitor.ko

PROBLEM: Supervisor crashes and socket file is left behind
FIX:     rm -f /tmp/mini_runtime.sock
         Then restart: sudo ./engine supervisor ./rootfs-base

PROBLEM: Two containers using same rootfs-alpha cause filesystem conflicts
FIX:     Each container MUST get its own rootfs copy:
         cp -a rootfs-base rootfs-alpha
         cp -a rootfs-base rootfs-beta
         Never share a writable rootfs between two running containers.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
QUICK REFERENCE — COMMAND CHEAT SHEET
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Build:
  make all                         # build engine + monitor.ko
  make clean                       # remove all build artifacts

Module:
  sudo insmod monitor.ko           # load kernel module
  sudo rmmod monitor               # unload kernel module
  lsmod | grep monitor             # check if loaded
  dmesg | tail -10                 # see kernel messages

Supervisor:
  sudo ./engine supervisor ./rootfs-base   # start (Terminal 1)

Containers:
  sudo ./engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]
  sudo ./engine run   <id> <rootfs> <cmd>   # blocks until exit
  sudo ./engine ps                          # list all containers
  sudo ./engine logs  <id>                  # view container log
  sudo ./engine stop  <id>                  # stop a container

Monitoring:
  sudo dmesg --follow | grep container_monitor    # watch kernel events
  tail -f logs/<id>.log                           # watch container logs
  watch -n 2 'ps aux | grep engine'               # watch processes
  ps aux | grep defunct                           # check for zombies
