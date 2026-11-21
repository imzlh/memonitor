# MemMonitor

一个面向 Linux 的轻量 C++/Qt 6 原生桌面内存监视器。它不使用 WebUI、WebView 或浏览器窗口。

## 监视口径

- 物理内存：`/proc/meminfo` 的 `MemTotal`、`MemAvailable`，已用为两者之差；同时显示 `MemFree`。估算可用包含部分可回收缓存，不是精确页分配清单，也不保证下一次内存申请成功。
- 进程内存：优先读取 `/proc/<pid>/smaps_rollup` 的 `Pss`、`SwapPss`、`Rss` 和 `Swap`。
- 兼容性回退：无法读取 `smaps_rollup` 时使用 `VmRSS`/`VmSwap`，只有精确 `PSS` 才能触发实际 RAM 告警；近似采样不会触发告警，详细采样状态可在窗口底部状态提示中查看。
- zram：读取每个 `/sys/block/zram*/mm_stat`，显示 `orig_data_size`、`compr_data_size` 和 `mem_used_total`。
- 内存压力：读取 `/proc/pressure/memory` 的 `some/full avg10`，显示最近 10 秒平均内存阻塞时间比例；同时显示 `SwapFree` 和 `/proc/vmstat` 的开机以来 `oom_kill` 进程计数。缺失数据明确标为不可读。zram 的累计写失败次数来自 `io_stat`，可在设备或概览提示中查看。

左栏直接显示整体内存概览，饼图居中并限制最大直径，图例以两列小字放在图下。主视图按窗口和进程父子关系排列；KDE Plasma 下由 KWin 提供窗口标题，窗口标题只显示在应用行，展开后的子进程行显示各自进程名，不重复窗口标题或应用图标。滚动期间会暂缓后台进程树更新，停止滚动后合并刷新。默认只显示当前用户的进程；进程区右上角的唯一搜索框可按进程名、PID、用户、命令行和窗口标题搜索，用户范围过滤在设置中选择。右键菜单支持结束单个进程或其进程树；发送信号前会用 `pidfd` 和进程启动时间复核身份。驻留 PSS、逻辑 SwapPss 和 `PSS + SwapPss` 足迹均可排序。PSS 会按比例分摊共享页，避免把共享库重复算给每个进程。`SwapPss` 是进程在交换区的逻辑归属量，不是压缩后的 RAM 字节；zram 的压缩比例只能由设备整体的 `mm_stat` 得到，Linux 标准接口没有提供逐进程的压缩后归属量。

表格以“内存、交换、总足迹”显示 PSS、SwapPss 和两者之和，悬停表头可查看口径；无法取得精确值时在对应数字前显示 `≈`，应用汇总中包含近似值也会标记，共享页可能重复。内存单元格下的细条表示该项占物理内存总量的比例。小窗口会保留名称列的宽度，并提供横向滚动。应用主行可直接右键结束应用及其子进程；筛选范围外的祖先进程以淡色名称保留层级，不计入统计。

`Ctrl+F` 切回进程页并定位搜索框，`Esc` 清空搜索；按窗口标题搜索时会同时显示其所属进程，清空后恢复搜索前的展开和滚动位置。无匹配时显示明确的空结果提示。环形图外圈按物理 RAM 总量显示估算可用、其他已用、zram 实际 RAM，中心百分比为物理已用；内圈按 zram 设备逻辑容量显示压缩前数据使用率，两圈分别计算。逻辑数据不能与物理总量相加；交换槽位实际余量以交换区指标为准，可能与设备内现存压缩数据量不同。zram 设备页刷新会保留当前选中的设备。

右上角的设置按钮（`Ctrl+,`）提供刷新间隔（500–10000 ms）、进程范围（我的进程、所有进程、其他用户 / 系统）、内存告警通知和关闭到托盘设置；保存后即时生效，重启后保留。进程表按 PID 和启动时间复用现有行，刷新保留展开、选中和滚动位置；列宽可手动拖动，不会在采样时逐单元格反复测宽。

窗口操作整合在第一排右侧，使用 Windows 式横线、方框、重叠方框和叉号，关闭按钮悬停显示红色背景。标题栏高 40 px，窗口按钮紧贴上边和右边，内容区单独保留边距。标题采用常规字重的 Noto Sans Mono 等宽字体，缺失时回退到 Hack 或系统等宽字体。拖动标题或第一排空白处可移动窗口，双击可切换最大化，边缘和四角可调整大小；移动和缩放交由桌面窗口管理器处理，支持 KDE 的窗口吸附。关闭按钮仍遵循设置中的关闭到托盘选项。

zram 的 `mem_used_total` 是物理 RAM 中的 zram 分配量，程序单独显示它，不会把它和 `MemTotal - MemAvailable` 做二次相加。zram 页的逐进程压缩后 RAM 归属无法从 Linux 标准接口精确得到。

zram 快满意味着继续换出匿名页的余地减少，但不能仅凭这一点解释“估算可用仍有数 GiB 时 OOM”。需区分内核 OOM 击杀、cgroup 限额、应用堆或地址空间上限、分配失败，以及采样错过的瞬时压力。内核 OOM 击杀计数不包含应用自身报错或用户态程序终止进程。PSI 描述已发生的停顿，也不是未来可分配容量的预测；本程序不会将这些指标合成一个没有明确物理意义的“安全可用内存”。

当任一进程的精确驻留 `PSS` 严格高于物理内存总量的 30% 时，程序通过 Qt 托盘或 freedesktop D-Bus 通知发出一次警告；进程降回阈值以下后再次超过阈值会重新通知。SwapPss 只作为逻辑交换量展示，不参与这个“实际 RAM”告警。

## 构建

Debian/Ubuntu：

```sh
sudo apt install qt6-base-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

启动原生窗口：

```sh
./build/memonitor
```

不启动窗口、只输出一次采样（用于排查权限或内核接口）：

```sh
./build/memonitor --dump
```

默认关闭主窗口会隐藏到系统托盘，托盘菜单中的“退出”会结束后台监视。可在设置中关闭这一行为；若桌面没有托盘，关闭窗口也会直接退出。系统通知优先使用 Qt 托盘消息，否则尝试 freedesktop Notifications D-Bus。

## deb 安装包与 GitHub Actions

`.github/workflows/deb.yml` 会在分支推送、Pull Request 和手动触发时自动构建以下架构：

| deb 架构 | 适用设备 | 构建方式 |
| --- | --- | --- |
| `amd64` | Intel / AMD 64 位电脑 | 原生构建并运行测试 |
| `arm64` | ARM 64 位电脑、开发板 | 原生构建并运行测试 |
| `armhf` | ARMv7 32 位设备 | QEMU 构建 |
| `riscv64` | RISC-V 64 位设备 | QEMU 构建 |

统一在 Debian 13（trixie）容器内编译，使用系统 Qt 6 动态库，并自动生成运行时依赖。安装目标需提供相应版本的依赖；不支持直接安装到 Debian 12 或较旧的 Ubuntu。每个架构都会在全新容器内安装生成的 deb，检查 `--dump` 和无显示器 GUI 启动。QEMU 架构不运行包含性能阈值的完整测试集。

构建完成后，在 Actions 运行页面下载 `deb-<架构>` artifact，内含 `.deb` 和对应的 SHA-256 校验文件。推送与 `CMakeLists.txt` 中项目版本一致的标签（例如 `v0.1.0`）时，全部架构成功后会自动创建或更新 GitHub Release，附上四个 deb 和 `SHA256SUMS.txt`。标签版本不匹配会使构建失败。

安装下载的包：

```sh
sudo apt install ./memonitor_0.1.0_amd64.deb
```

安装后可从应用菜单打开 MemMonitor，也可运行 `memonitor`。

在 Debian 13 本地复用同一打包脚本：

```sh
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev dpkg-dev file dbus-daemon
bash packaging/build-deb.sh
```

默认使用 `build-deb/` 构建、输出到 `dist/`，并运行测试；可用 `BUILD_TESTING=OFF` 关闭测试，用 `CMAKE_BUILD_PARALLEL_LEVEL` 调整并行数（默认 2）。

