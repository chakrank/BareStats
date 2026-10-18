# Arch Linux Package (PKGBUILD)

This directory contains the Arch Linux packaging recipe for BareStats.

## Building Locally with makepkg

To build and install the package on Arch Linux, Manjaro, or EndeavourOS:

```bash
# 1. Install prerequisites
sudo pacman -S --needed base-devel git cmake ninja pkgconf

# 2. Build and install package
cd packaging/arch
makepkg -si
```

## Installing Generated Package

After building, the package (`.pkg.tar.zst`) can be installed with:

```bash
sudo pacman -U barestats-1.3.0-1-x86_64.pkg.tar.zst
```
