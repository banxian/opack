# SquashFS 镜像制作工具

这是一个用于制作兼容于 RG300X 等 GCW0 兼容机型的 SquashFS 镜像的工具.

因为是通过阅读和分析文件格式撰写的, 没有传染 GPL 授权.

## 特性

- 默认生成较旧的 SquashFS 格式。
- 可运行于 Windows XP。
- 使用 Zopfli 压缩，提供比 Zlib 更好的压缩率。

## 运行参数

```bash
opack 输入文件夹 输出镜像.opk 可选选项
```

- -no-tailends 不将大文件末尾合入碎片
- -no-autoexec 关闭自动给ELF文件加权限功能
- -real-time 使用实际的文件时间

## 如何编译

可以使用以下工具链:

- WDK7
- VC2010
- VC2015
- VC2017

### 使用 VC2010 编译

1. 将 `opack.c` 改名为 `opack.cpp`.
2. 或者选中该文件, 单独在编译选项中将 `Compile As` 设置为 `C++`, 因为 VC2010 不支持 C99.

### 使用 VC2015/VC2017 编译

ReleaseXP配置的编译选项中默认加入了 `/d2noftol3`, 该选项是在高版本VC配合WDK的msvcrt.dll的.

不需要改名 `opack.c` 或者设置 `Compile As` 选项.

### 注意事项

- `ReleaseXP` 选项是链接到 `msvcrt.dll`. 此配置下请修改包含路径, 或者在MSBuild里面复制toolset, 修改VC20xx-WDK配置.
- `Release` 选项是链接到对应版本的VC运行库, 在XP运行时候需要安装对应的带版本号运行库.

## RG300x对opk文件的要求

- 只支持gzip压缩格式, 不支持lzo/lzma/xz/zstd的压缩格式.
- 可执行文件需要执行权限, 其他文件权限可以为空.