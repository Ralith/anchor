# Maintainer: Benjamin Saunders <ben.e.saunders@gmail.com>
pkgname="anchor"
pkgver=0.0.17
pkgrel=1
pkgdesc="Multi-connection HTTP file downloading utility"
arch=('i686' 'x86_64')
url="https://github.com/Ralith/$pkgname"
license=('MIT')
depends=('libuv' 'c-ares' 'gcc-libs')
makedepends=('clang' 'tup')
source=("$pkgname"::"git://github.com/Ralith/$pkgname.git"
        "http-parser"::"git://github.com/joyent/http-parser.git")
md5sums=('SKIP' 'SKIP')
options=('strip')

pkgver() {
    cd "$srcdir/$pkgname"
    echo -n 0.0.
    git rev-list --count HEAD
}

prepare() {
    cd "$srcdir/$pkgname"
    git submodule init
    git config submodule.http-parser.url "$srcdir/http-parser"
    git submodule update
}

build() {
    cd "$srcdir/$pkgname"
    tup
}

package() {
  install -Dm755 "$srcdir/$pkgname/$pkgname" "$pkgdir/usr/bin/$pkgname"
}
