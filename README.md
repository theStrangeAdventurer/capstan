### Как собрать скачанный ncurses

```sh
cd vendor/ncurses-src
./configure \
  --with-shared \
  --with-termlib \
  --without-tests \
  --prefix=$(pwd)/../ncurses-install
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
