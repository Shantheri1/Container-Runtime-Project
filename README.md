# 🐳 Mini Container Runtime (OS Project)

## 📌 Overview

This project implements a **basic container runtime** in C using core Linux concepts:

* Process isolation (namespaces)
* Resource control (cgroups)
* Kernel monitoring (custom module)

It simulates how tools like Docker manage containers at a low level.

---

## ⚙️ Features

* Create and manage containers using a custom `engine`
* Memory limit enforcement (soft & hard limits)
* CPU scheduling using `nice` values
* Kernel-level monitoring using a custom module
* Logging system for each container
* Stress tools:

  * `cpu_hog` (CPU intensive)
  * `memory_hog` (memory intensive)

---

## 🧱 Project Structure

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

### 6. View logs

```bash
sudo ./engine logs alpha
```

---

## 📊 Observations

* Lower nice value → higher CPU priority
* Memory exceeding soft limit → warning (dmesg)
* Memory exceeding hard limit → process killed
* Some processes may become `<defunct>` if not cleaned

---

## 🧠 Key Concepts Used

* `fork()`, `exec()`, `wait()`
* `chroot()` for filesystem isolation
* `ioctl()` for user-kernel communication
* Linux scheduling (`nice`)
* Process states (Running, Zombie, etc.)

---

## ⚠️ Notes

* Run commands with `sudo` where required
* Kernel module must be loaded before using supervisor
* Avoid committing binaries and logs


