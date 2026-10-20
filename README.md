# 欣心相印 · 终端闪烁爱心

单文件 C11 程序：在终端里绘制一颗会双跳（lub-dub）、发光、闪烁的爱心，心口嵌入
“欣心相印”。纯标准 C 运行时 + 系统终端 API，无第三方库。

![预览](heart_preview.png)

> 预览图由程序实际输出的 ANSI 帧转换而来，字体与真实终端略有差异。

## 编译

Windows（MinGW-w64 GCC，已在 GCC 16.2 上验证）：

```bat
gcc -std=c11 -O2 -Wall -Wextra -pedantic heart.c -o heart.exe -lm
```

Linux / macOS：

```sh
gcc -std=c11 -O2 -Wall -Wextra -pedantic heart.c -o heart -lm
```

源码文件必须保持 UTF-8 编码，因为其中包含“欣心相印”的 UTF-8 字面量。

## 运行

```bat
heart.exe
```

默认动画播放 12 秒后自动退出并恢复终端（光标、备用屏幕、控制台模式）。
想一直看：

```bat
heart.exe --loop
:: 或 heart.exe --duration 0
```

只输出一帧（适合截图或终端不支持动画时）：

```bat
heart.exe --static
heart.exe --static --size 100x36
```

## 效果说明

- **形状**：隐函数 `(x² + y² - 1)³ - x²y³ ≤ 0` 生成经典爱心轮廓，用距离场做抗锯齿。
- **清晰度**：每个字符格用 `▀`（U+2580）把前后景色分别当作上下两个像素，纵向分辨率翻倍。
- **心跳**：`heartbeat()` 用两个高斯脉冲模拟“咚—哒”双跳，驱动尺寸、亮度和外发光。
- **颜色**：24 位真彩色；深红底部、洋红中部、珊瑚粉上部，边界有霓虹描边，心腔有流动高光。
- **氛围**：深紫黑渐变背景、细微星尘、随机闪烁粒子；粒子相位和位置由固定种子生成，效果可复现。
- **文字**：心形中央的深色圆角铭牌内显示“欣心相印”，文字为 UTF-8，宽度按东亚宽字符处理。
- **终端恢复**：正常结束或 Ctrl+C 时恢复光标、退出备用屏幕并还原 Windows 控制台模式。

## 常用参数

| 参数 | 默认 | 说明 |
|---|---:|---|
| `-d, --duration SEC` | `12` | 播放时长（秒）；`0` 表示无限循环 |
| `-f, --fps N` | `24` | 帧率，影响流畅度而非心跳速度 |
| `-b, --bpm N` | `72` | 每分钟心跳次数（20–240） |
| `-p, --sparkles N` | `28` | 闪烁粒子数量（0–200） |
| `-s, --size WxH` | 自动 | 强制字符格尺寸，例如 `100x36` |
| `-c, --color MODE` | `truecolor` | `truecolor` / `256` / `16` / `none` |
| `--no-color` | 关 | 关闭颜色，自动使用 ASCII 字符画 |
| `-a, --ascii` | 关 | 用 ASCII 字符代替 Unicode 半块 |
| `--static` | 关 | 只渲染一帧并退出 |
| `--no-cjk` | 关 | 把铭牌文字换成 `XIN XIN` |
| `--plain` | 关 | `--static --ascii --no-color --no-cjk` |
| `--loop` | 关 | 无限循环 |

示例：

```bat
heart.exe --bpm 84 --sparkles 40
heart.exe --duration 6 --color 256
heart.exe --static --ascii --color truecolor
```

## 修改入口

- `HEART_TEXT`：铭牌文字（UTF-8）。
- `DEFAULT_DURATION` / `DEFAULT_FPS` / `DEFAULT_BPM` / `DEFAULT_SPARKLES`：默认值。
- `HEART_BEAT_SCALE_X` / `HEART_BEAT_SCALE_Y`：心跳时的心形缩放幅度。
- `HEART_RIM_WIDTH`：边缘霓虹宽度。
- `heart_field()`：心形隐函数（换公式即可换形状）。
- `heartbeat()`：双脉冲节奏。
- `shade_pixel()`：主配色、光晕、高光和抗锯齿，颜色都带注释。
- `draw_text_plate()`：铭牌位置、大小、边框和文字颜色。

## 平台与降级

- **Windows 10/11**：推荐 Windows Terminal。程序通过
  `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` 启用 ANSI，并把输出代码页切到 UTF-8。
  旧控制台若不支持 VT，会自动降级为静态 ASCII 帧。
- **Linux/macOS**：使用 `ioctl(TIOCGWINSZ)` 获取窗口大小，使用 `clock_gettime` + `nanosleep`，
  ANSI 控制序列原生可用。
- 清除屏幕、光标隐藏、延时均已按平台分支处理。
- **颜色降级**：`--color none` 或设置 `NO_COLOR=1`（若显式传 `--color` 则优先命令行）。
  若终端支持 ANSI 但不支持真彩色，可用 `--color 256` 或 `--color 16`。
- **无颜色/无动画控制字符**：

  ```bat
  heart.exe --plain
  ```

  这条路径不输出任何 ANSI 转义序列，只打印一帧纯 ASCII（铭牌为 `XIN XIN`）。
- 若系统设置了 `NO_COLOR`，而仍想强制彩色：

  ```bat
  heart.exe --color truecolor
  ```

## 参考

实现前检索并阅读了以下公开实现：

- [T4ras123/ascii-love](https://github.com/T4ras123/ascii-love)：终端爱心动画，使用
  三维参数曲面、亮度字符与 ANSI 颜色。本程序借鉴了“心形亮度着色 + 帧重绘”的总体思路。
- [portfoliocourses/c-example-code: print_a_heart.c](https://github.com/portfoliocourses/c-example-code/blob/main/print_a_heart.c)：
  按行打印 ASCII 心形的基础示例，用于对照轮廓比例。

本实现没有直接复制上述仓库代码；形状场、半块渲染、心跳包络、铭牌、粒子、
Windows/POSIX 终端适配和降级逻辑均为独立编写。

## 已验证

- `gcc -std=c11 -O2 -Wall -Wextra -Werror -pedantic` 零警告通过。
- `--static`、`--plain`、`--color truecolor/256/16/none`、`--size 20x6`、`--size 200x80`
  均正常退出。
- 动画在 1 秒测试时长内输出完整帧，并发送了备用屏幕进入/退出、隐藏/恢复光标序列。
- `TERM=dumb` 与 `--plain` 输出中 ANSI ESC 数量为 0。
