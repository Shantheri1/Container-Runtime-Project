#  Mini Container Runtime (OS Project)

## Overview

This project implements a **basic container runtime** in C using core Linux concepts:

* Process isolation (namespaces)
* Resource control (cgroups)
* Kernel monitoring (custom module)

It simulates how tools like Docker manage containers at a low level.

---

##  Features

* Create and manage containers using a custom `engine`
* Memory limit enforcement (soft & hard limits)
* CPU scheduling using `nice` values
* Kernel-level monitoring using a custom module
* Logging system for each container
* Stress tools:

  * `cpu_hog` (CPU intensive)
  * `memory_hog` (memory intensive)

---

##  Project Structure

```
boilerplate/
├── engine.c              # Container runtime
├── monitor.c             # Kernel monitoring logic
├── monitor_mod.c         # Kernel module interface
├── monitor_ioctl.h       # Communication (ioctl)
├── cpu_hog.c             # CPU stress program
├── memory_hog.c          # Memory stress program
├── Makefile              # Build instructions
├── rootfs-alpha/         # Container filesystem (alpha)
├── rootfs-beta/          # Container filesystem (beta)
├── logs/                 # Container logs
```

---

## 🚀 How to Run

### 1. Build the project

```bash
make
```

### 2. Load kernel module

```bash
sudo insmod monitor.ko
```

### 3. Start supervisor

```bash
sudo ./engine supervisor ../rootfs-base
```

### 4. Start containers

```bash
sudo ./engine start alpha ../rootfs-alpha /cpu_hog --nice -5
sudo ./engine start beta ../rootfs-beta /cpu_hog --nice 10
```

### 5. Check status

```bash
sudo ./engine ps
```

**View logs:**
```bash
sudo ./engine logs alpha
sudo ./engine logs beta
```

**Stop containers:**
```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Step 7: Run memory test workload
```bash
gcc -static -o memory_hog memory_hog.c
sudo cp memory_hog ./rootfs-alpha/
sudo cp memory_hog ./rootfs-beta/

sudo ./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 10 --hard-mib 30
sudo ./engine start beta ./rootfs-beta /memory_hog --soft-mib 10 --hard-mib 30
```

### Step 8: Run scheduling experiment
```bash
gcc -static -o cpu_hog cpu_hog.c
sudo cp cpu_hog ./rootfs-alpha/
sudo cp cpu_hog ./rootfs-beta/

sudo ./engine start alpha ./rootfs-alpha /cpu_hog --nice -5
sudo ./engine start beta ./rootfs-beta /cpu_hog --nice 10

sleep 3
ps -eo pid,ni,%cpu,comm | grep cpu_hog
```

### Step 9: Check kernel logs
```bash
sudo dmesg | grep container_monitor
```

### Step 10: Cleanup
```bash
sudo ./engine stop alpha
sudo ./engine stop beta
ps aux | grep defunct
sudo rmmod monitor
sudo dmesg | tail -5
```


##  Observations

* Lower nice value → higher CPU priority
* Memory exceeding soft limit → warning (dmesg)
* Memory exceeding hard limit → process killed
* Some processes may become `<defunct>` if not cleaned

---

##  Key Concepts Used

* `clone()`, `exec()`, `wait()`
* `chroot()` for filesystem isolation
* `ioctl()` for user-kernel communication
* Linux scheduling (`nice`)
* Process states (Running, Zombie, etc.)

---

## ⚠️ Notes

* Run commands with `sudo` where required
* Kernel module must be loaded before using supervisor
* Avoid committing binaries and logs


