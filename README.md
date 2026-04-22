
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




## 📸 Demo with Screenshots

### 1. Multi-container supervision

![Multi-container supervision output](https://github.com/user-attachments/assets/a47348eb-7c30-4388-a334-14fa80db9cc5)

 Multiple containers (`alpha`, `beta`) managed simultaneously by a single supervisor process. This demonstrates concurrent container execution and supervision.

---

### 2. Metadata tracking

![Metadata tracking output](https://github.com/user-attachments/assets/d9d2fa27-2ed3-41ce-ac53-ff6510fa8250)

 Output of `engine ps` showing container ID, PID, state (running/exited), and lifecycle information, confirming proper metadata management.

---

### 3. Bounded-buffer logging

![Logs from alpha container](https://github.com/user-attachments/assets/143f775e-07d7-435a-a36d-65d3ad069800)

![Logs from beta container](https://github.com/user-attachments/assets/853ab99e-d0e8-4ea6-9fbe-78ab25c7623e)

 Logs from containers `alpha` and `beta` captured via pipe-based IPC and stored independently. This demonstrates the bounded-buffer producer–consumer logging mechanism.

---

### 4. CLI and IPC

![Supervisor CLI and IPC](https://github.com/user-attachments/assets/150faf18-652f-46d1-98fd-aef8484f6b1e)

 Interaction between CLI and supervisor showing container lifecycle commands (`start`, `stop`). Demonstrates IPC-based communication.

---

### 5. Soft-limit warning

![Soft memory limit warning](https://github.com/user-attachments/assets/2bf3a7bc-309a-4c06-86c5-17e7b6a57e99)

 Kernel logs showing soft memory limit exceeded. The process continues execution but a warning is generated.

---

### 6. Hard-limit enforcement

![Hard memory limit enforcement](https://github.com/user-attachments/assets/150168a9-78a3-4fa5-8018-a582377fdf60)

 Kernel logs showing enforcement of hard memory limits where the container process is terminated after exceeding allowed memory.

---

### 7. Scheduling experiment

![CPU scheduling experiment output](https://github.com/user-attachments/assets/58b5e7db-0525-4b1b-bbc8-20036eb264cf)

 CPU scheduling behavior using different `nice` values. Lower nice value results in higher CPU priority, validating Linux scheduler behavior.

---

### 8. Clean teardown

![Cleanup and no zombie processes](https://github.com/user-attachments/assets/087d100e-22c8-40fa-a019-d59d5f4212d7)

 After stopping containers, no `<defunct>` (zombie) processes are present. Confirms proper resource cleanup and child process reaping.

---











##  Key Concepts Used

* `clone()`, `exec()`, `wait()`
* `chroot()` for filesystem isolation
* `ioctl()` for user-kernel communication
* Linux scheduling (`nice`)
* Process states (Running, Zombie, etc.)

---

## Notes

* Run commands with `sudo` where required
* Kernel module must be loaded before using supervisor
* Avoid committing binaries and logs

 ##Authors
 Shriranjini[PES1UG24CS447]
 Shantheri Shenoy[PES1UG24CS428]


