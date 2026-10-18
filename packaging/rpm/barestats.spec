Name:           barestats
Version:        1.3.0
Release:        1%{?dist}
Summary:        BareStats - Ultra-lightweight native system telemetry overlay

License:        MIT
URL:            https://github.com/chakrank/BareStats
Source0:        %{name}-%{version}.tar.gz
Packager:       Chakrank <chakrank@hotmail.com>
Vendor:         Chakrank

BuildRequires:  gcc-c++
BuildRequires:  cmake >= 3.20
BuildRequires:  ninja-build
BuildRequires:  pkgconfig
BuildRequires:  cairo-devel
BuildRequires:  libX11-devel
BuildRequires:  libXfixes-devel
BuildRequires:  libXcomposite-devel
BuildRequires:  libXrender-devel
BuildRequires:  wayland-devel

Requires:       cairo
Requires:       libX11
Requires:       libXfixes
Requires:       libXcomposite
Requires:       libXrender

%description
BareStats is an unprivileged, ultra-lightweight telemetry overlay delivering
real-time CPU, GPU, VRAM, RAM, and network throughput at 0.00% idle duty cycle.
Engineered for local AI/ML workloads, competitive gaming, and workstation productivity.
Developed by Chakrank.

%prep
%autosetup

%build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBARESTATS_BUILD_TESTS=OFF
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md CHANGELOG.md config.example.ini
%{_bindir}/barestats
%{_datadir}/applications/barestats.desktop
%{_datadir}/pixmaps/barestats.png
%{_datadir}/icons/hicolor/256x256/apps/barestats.png
%{_datadir}/icons/hicolor/scalable/apps/barestats.svg
%{_datadir}/metainfo/io.github.chakrank.barestats.metainfo.xml
%{_datadir}/doc/barestats/*

%changelog
* Fri Oct 16 2026 Chakrank <chakrank@hotmail.com> - 1.3.0-1
- Release BareStats v1.3.0 cross-platform Linux support with XWayland overlay and multi-GPU monitoring.
