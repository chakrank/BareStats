Name:           barestats
Version:        1.4.0
Release:        1%{?dist}
Summary:        BareStats - Ultra-lightweight native system telemetry overlay

License:        MIT
URL:            https://github.com/chakrank/BareStats
Source0:        %{name}-%{version}.tar.gz
Packager:       Chakrank <chakrank@outlook.com>
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

Provides:       barestats = %{version}-%{release}
Provides:       barestats(x86-64) = %{version}-%{release}
Provides:       application()
Provides:       application(barestats.desktop)
Provides:       appdata()
Provides:       appdata(io.github.chakrank.barestats.metainfo.xml)
Provides:       metainfo()
Provides:       metainfo(io.github.chakrank.barestats.metainfo.xml)

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

%post
# Remove legacy desktop file from earlier versions if present
rm -f %{_datadir}/applications/io.github.chakrank.barestats.desktop
if [ -x %{_bindir}/update-desktop-database ]; then
    %{_bindir}/update-desktop-database -q %{_datadir}/applications || true
fi
if [ -x %{_bindir}/gtk-update-icon-cache ]; then
    %{_bindir}/gtk-update-icon-cache -q -t -f %{_datadir}/icons/hicolor || true
fi

%preun
if [ $1 -eq 0 ]; then
    # Complete package removal (not an upgrade)
    killall -q barestats 2>/dev/null || pkill -x barestats 2>/dev/null || true
fi

%postun
if [ -x %{_bindir}/update-desktop-database ]; then
    %{_bindir}/update-desktop-database -q %{_datadir}/applications || true
fi
if [ -x %{_bindir}/gtk-update-icon-cache ]; then
    %{_bindir}/gtk-update-icon-cache -q -t -f %{_datadir}/icons/hicolor || true
fi

%files
%license LICENSE
%doc README.md CHANGELOG.md config.example.ini
%{_bindir}/barestats
%{_datadir}/applications/barestats.desktop
%{_datadir}/pixmaps/*
%{_datadir}/icons/hicolor/*/apps/*
%{_datadir}/metainfo/io.github.chakrank.barestats.metainfo.xml
%{_datadir}/doc/barestats/*

%changelog
* Tue Oct 20 2026 Chakrank <chakrank@outlook.com> - 1.4.0-1
- Release BareStats v1.4.0 with Distributed AI Inference Telemetry, Satellites, and Monocypher encryption.

* Fri Oct 16 2026 Chakrank <chakrank@outlook.com> - 1.3.0-1
- Release BareStats v1.3.0 cross-platform Linux support with XWayland overlay and multi-GPU monitoring.
