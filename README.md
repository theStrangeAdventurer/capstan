### Как собрать скачанный ncurses

```sh
cd vendor/ncurses-src
./configure \
  --with-shared \
  --with-termlib \
  --without-tests \
  --prefix=$(pwd)/../ncurses-install
```

### Как скачать и собрать lua

> [link](https://www.lua.org/download.html)

```sh
cd vendor
curl -L -R -O https://www.lua.org/ftp/lua-5.5.0.tar.gz
tar zxf lua-5.5.0.tar.gz
cd lua-5.5.0
make all test

```

#### Build

##### Linux
make -j$(nproc)

##### MacOS
make -j$(sysctl -n hw.ncpu)

make install

#### Compile_commands

```sh
bear -- make
```
