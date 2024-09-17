# SquashFS 镜像制作工具

这是一个用于制作兼容于 RG300X 等 GCW0 兼容机型的 SquashFS 镜像的工具.

## 特性

- 默认生成较旧的 SquashFS 格式。
- 可运行于 Windows XP。
- 使用 Zopfli 压缩，提供比 Zlib 更好的压缩率。

## 运行参数

```bash
opack 输入文件夹 输出镜像.opk
```

## 如何编译

可以使用以下编译工具：

- WDK7
- VC2010
- VC2015
- VC2017

### 使用 VC2010 编译

1. 将 `opack.c` 改名为 `opack.cpp`.
2. 或者单独选中该文件, 在编译选项中将 `Compile As` 设置为 `C++`, 因为 VC2010 不支持 C99.

### 使用 VC2015/VC2017 编译

如果使用 VC2015/VC2017 编译器, 请确保在编译选项中添加 `/d2noftol3`, 否则可能会出现找不到符号的错误.

直接使用 C 编译

### 注意事项

- `ReleaseXP` 选项是链接到 `msvcrt.dll`.
- `Release` 选项是链接到对应版本的VC运行库, 在XP运行时候需要安装对应的运行库.

