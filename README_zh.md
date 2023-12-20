# trimcheck_linux
检查你的ssd的trim功能是否正常。

# 原理
一句话概括：新建文件，记录偏移，删除文件，触发Trim，读取偏移，验证校验和。  

# 构建
你需要安装`cmake`和`zlib1g-dev`包：
```bash
sudo apt install cmake zlib1g-dev
```
然后，开始构建：
```bash
mkdir build
cd build
cmake ..
make
```

# 使用
首先使用`lsblk -D`查询磁盘是否支持TRIM:
```bash
lsblk -D /dev/sda
```
如果DISC-MAX一列为0,则代表磁盘不支持TRIM。

挂载磁盘：
```bash
sudo mount /dev/sda1 /mnt
```

运行程序:
```bash
sudo cp trimcheck /mnt
cd /mnt
sudo ./trimcheck
```
程序运行完成后会在当前目录下生成`trimcheck_report.txt`。

等待一段时间后，再次运行程序以验证TRIM是否工作。
在我的机器上，TRIM必须等待2-3个小时后才能生效。

# 命令行参数
```bash
使用方法: ./trimcheck <选项>
选项:
-h, --help              显示此帮助
-v, --verbose           详细输出
-o, --offset            指定文件偏移量，与 -c 参数配合使用
-d, --partition         指定读取的分区, 与 -c 参数配合使用
-s, --size              指定文件大小, 默认 1 MB (1048576 字节)
-c, --checksum          读取指定的块，并且计算校验和
-n, --name              指定要生成的文件名
-N, --no-trim           删除文件后不要对磁盘执行TRIM操作
-f, --report-file       指定报告文件名，默认为 "trimcheck_report.txt"
-w, --wait-time         指定等待文件写入的时间，默认为10秒
```