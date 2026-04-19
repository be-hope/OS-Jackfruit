# OS-Jackfruit: Multi-Container Runtime with Kernel Monitoring

# 1. Team Information

## Team Members

* Bhadra RS  — SRN: PES1UG24CS111
* Arundhathi K — SRN: PES1UG24CS082




---

# 2. Project Overview

This project implements a lightweight Linux container runtime featuring:

* Multi-container supervision
* PID / UTS / mount namespace isolation
* Separate writable root filesystems
* CLI control commands (`start`, `run`, `ps`, `logs`, `stop`)
* IPC between CLI and supervisor
* Bounded-buffer concurrent logging
* Kernel memory monitor module
* Soft-limit warning and hard-limit enforcement
* Scheduling experiments

---

# 3. Build, Load, and Run Instructions

## Environment

* Ubuntu 22.04 / 24.04
* GCC
* make
* Linux kernel headers

---

## Step 1 — Build

```bash
cd ~/OS-Jackfruit/boilerplate
make
```

Builds:

* `engine`
* `monitor.ko`
* `cpu_hog`
* `io_pulse`
* `memory_hog`

---

## Step 2 — Load Kernel Module

```bash
sudo insmod monitor.ko
```

Verify:

```bash
lsmod | grep monitor
ls -l /dev/container_monitor
```

---

## Step 3 — Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

Expected:

```text
Supervisor running...
```

---

## Step 4 — Create Writable Rootfs Copies

```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

---

## Step 5 — Copy Workloads

```bash
cp cpu_hog rootfs-alpha/
cp cpu_hog rootfs-beta/
cp io_pulse rootfs-beta/
cp memory_hog rootfs-alpha/
```

---

## Step 6 — Launch Containers

Open second terminal:

```bash
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /cpu_hog --soft-mib 64 --hard-mib 96
```

---

## Step 7 — View Metadata

```bash
sudo ./engine ps
```

---

## Step 8 — View Logs

```bash
sudo ./engine logs alpha
```

---

## Step 9 — Memory Limit Test

```bash
sudo ./engine start mem1 ./rootfs-alpha /memory_hog
sudo dmesg | grep container_monitor
```

---

## Step 10 — Scheduling Experiment

```bash
sudo ./engine start cpu1 ./rootfs-alpha /cpu_hog --nice 0
sudo ./engine start cpu2 ./rootfs-beta /cpu_hog --nice 10
top
```

---

## Step 11 — Stop Containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

---

## Step 12 — Unload Module

```bash
sudo rmmod monitor
```

---

# 4. Demo with Screenshots

All screenshots are stored inside:

```text
Screenshots/
```

---

## 1. Multi-container Supervision

### Screenshot A

![Multi Container 1](Screenshots/1_multicontainer_1.png)

### Screenshot B

![Multi Container 2](Screenshots/1_multicontainer_2.png)

Caption: Two or more containers managed under one supervisor process.

---

## 2. Metadata Tracking

![PS Output](Screenshots/2_ps_output.png)

Caption: `engine ps` showing container IDs, PIDs, and states.

---

## 3. Bounded-buffer Logging

![Logging](Screenshots/3_logging.png)

Caption: Container stdout/stderr captured through producer-consumer logging pipeline.

---

## 4. CLI and IPC

### Screenshot A

![CLI IPC 1](Screenshots/4_cli_ipc_1.png)

### Screenshot B

![CLI IPC 2](Screenshots/4_cli_ipc_2.png)

Caption: CLI commands sent to supervisor over IPC channel.

---

## 5 & 6. Soft-limit Warning + Hard-limit Enforcement

![Soft and Hard Limit](Screenshots/5_and_6_soft_and_hard_limit.png)

Caption: Soft-limit warning followed by hard-limit kill and unregister cleanup-please check the edited ss

---

## 7. Scheduling Experiment

### Screenshot A

![Scheduling 1](Screenshots/7_scheduling_1.png)

### Screenshot B

![Scheduling 2](Screenshots/7_scheduling_2.png)

Caption: CPU-bound workload consuming high CPU share under Linux scheduler.

---

## 8. Clean Teardown

![Teardown](Screenshots/8_scheduling.png)

Caption: Containers stopped, reaped correctly, and runtime cleaned up.

---

# 5. Engineering Analysis

## A. Process Isolation

Linux namespaces isolate PID space, hostname, and mount tree so each container appears independent while sharing the host kernel.

## B. Supervisor Design

A persistent parent supervisor tracks container metadata, handles commands, and reaps exited children using SIGCHLD.

## C. IPC Model

Two IPC paths were used:

* Control path: CLI ↔ Supervisor
* Logging path: Pipes → bounded buffer → log writer

This avoids interference between command traffic and logging traffic.

## D. Memory Enforcement

The kernel module reads process RSS memory. Soft-limit crossings generate warnings; hard-limit crossings terminate the container.

## E. Scheduling Behavior

Linux CFS aims for fairness while still responding to priority hints (`nice`) and interactive sleeping tasks.

---

# 6. Design Decisions and Tradeoffs

## Namespace Isolation

Choice: `clone()` with namespaces

Tradeoff: More setup complexity

Reason: Realistic container isolation.

---

## Supervisor Architecture

Choice: Long-running daemon

Tradeoff: Requires IPC and cleanup logic

Reason: Supports multiple simultaneous containers.

---

## Logging Pipeline

Choice: Producer-consumer bounded buffer

Tradeoff: Synchronization complexity

Reason: Prevents blocking and supports concurrent logging.

---

## Kernel Monitor

Choice: Character device + `ioctl`

Tradeoff: Kernel debugging complexity

Reason: Clean container registration and policy enforcement.

---

## Scheduling Tests

Choice: Synthetic workloads (`cpu_hog`, `io_pulse`)

Tradeoff: Less realistic than production apps

Reason: Clear and measurable behavior.

---

# 7. Scheduler Experiment Results

| Workload | Nice Value | Observed CPU          |
| -------- | ---------- | --------------------- |
| cpu1     | 0          | ~99%                  |
| cpu2     | 10         | Lower share           |
| io1      | 0          | Bursty / intermittent |

## Observations

* CPU-bound workloads consume sustained CPU time.
* Lower nice values receive more favorable scheduling.
* I/O-bound tasks sleep frequently and remain responsive.

## Conclusion

Linux CFS balances fairness, responsiveness, and priority hints effectively.

---

# 8. Cleanup Procedure

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
ps aux | grep engine
sudo rmmod monitor
```

No zombie container processes remain after shutdown.

---

# 9. Repository Structure

```text
OS-Jackfruit/
├── README.md
├── Screenshots/
├── boilerplate/
│   ├── engine.c
│   ├── monitor.c
│   ├── Makefile
│   ├── rootfs-base/
│   └── workloads
```

---

# 10. Final Checklist

* [ ] Replace SRNs
* [ ] Verify screenshot rendering
* [ ] Verify commands on fresh VM
* [ ] Push final commit
