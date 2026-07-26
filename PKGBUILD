# Maintainer: texsd <texsd@users.noreply.github.com>

_pkgbase=mech-forza-kmod
_dkmsname=mechrevo-ec
pkgname=mech-forza-kmod-dkms-git
pkgver=r0.0000000
pkgrel=2
pkgdesc="Lightweight GX4HRXL ACPI EC platform driver (DKMS)"
arch=('x86_64')
url="https://github.com/minortex/mech-forza-kmod"
license=('GPL-2.0-or-later')
depends=('dkms')
makedepends=('git')
provides=('mech-forza-kmod-dkms')
conflicts=('mech-forza-kmod-dkms')
source=("$_pkgbase::git+$url.git")
sha256sums=('SKIP')

pkgver() {
  cd "$_pkgbase"
  printf 'r%s.%s' "$(git rev-list --count HEAD)" "$(git rev-parse --short=7 HEAD)"
}

package() {
  cd "$srcdir/$_pkgbase"

  local dkms_src="$pkgdir/usr/src/$_dkmsname-$pkgver"

  install -Dm644 Makefile "$dkms_src/Makefile"
  install -Dm644 mechrevo-ec.c "$dkms_src/mechrevo-ec.c"
  install -Dm644 mechrevo_ec_uapi.h "$dkms_src/mechrevo_ec_uapi.h"
  install -Dm644 60-mechrevo-ec.rules \
    "$pkgdir/usr/lib/udev/rules.d/60-mechrevo-ec.rules"

  cat >"$dkms_src/dkms.conf" <<EOF_DKMS
PACKAGE_NAME="$_dkmsname"
PACKAGE_VERSION="$pkgver"
BUILT_MODULE_NAME[0]="$_dkmsname"
DEST_MODULE_LOCATION[0]="/kernel/drivers/platform/x86"
MAKE[0]="make KDIR=/usr/lib/modules/\${kernelver}/build"
CLEAN="make KDIR=/usr/lib/modules/\${kernelver}/build clean"
AUTOINSTALL="yes"
EOF_DKMS
}
