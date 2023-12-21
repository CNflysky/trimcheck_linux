# trimcheck_linux
a small utility to verify your ssd's trim functionality.  
[简体中文(Simplified Chinese)](./README_zh.md)

# how it works
create file -> get its offset -> delete file -> trigger trim -> read offset -> compare result.

# build
install `cmake` and `zlib1g-dev` package:
```bash
sudo apt install cmake zlib1g-dev
```
build:  
```bash
mkdir build
cd build
cmake ..
make
```

# usage
use `lsblk -D` to query your disk supports trim or not:
```bash
lsblk -D /dev/sda
```
if the column DISC_MAX is 0, then it does not support trim.

then mount partition:  
```bash
sudo mount /dev/sda1 /mnt
```
run the program:
```bash
sudo cp trimcheck /mnt
cd /mnt
sudo ./trimcheck
```
it will generate a file named `trimcheck_report.txt` under current directory.  
you should waiting for a while to make sure trim takes effect.  
on my machine it takes 2-3 hours to make sure trim takes effect.

# CLI parameters
try ./trimcheck --help to see it.
