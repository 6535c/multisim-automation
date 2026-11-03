# Multisim 10 Automation

Windows 上的 Multisim 10.0.1.0 x86 原理图编辑和仿真接口。Python 与 JSON 配方使用 Multisim 的原生编辑器和求解器。

- 创建可在 Multisim 中编辑的 .ms10 图纸，支持元件放置、引脚连线、折点、接点、网名、移动、旋转、镜像和单元件复制。
- 求解 DC 工作点、瞬态和 AC 扫频，保存原生网表和数值结果。plot_result.py 可生成瞬态与 AC 幅值 SVG。
- 支持选定的原生仪器和静态探针。总线支持主干放置。复杂器件模型、总线成员与多页层级需要进一步验证。

## 开始使用

解压完整发布包。安装 Multisim 10.0.1.0 x86 并确认 32 位 COM 可用。Python 3.10 或更新版本。

```powershell
$env:MULTISIM_HOME = 'D:\Multisim10\Circuit Design Suite 10.0'
python multisim.py run examples/voltage_divider.json --output divider.ms10
python multisim.py solve divider.ms10 --analysis op --output-dir op
python verify_solve.py
```

包内附带 x86 桥接 DLL 和 EXE。可用 Visual Studio x86 开发命令提示符运行 build_native.cmd 重新编译。包需要本机安装 Multisim 和其原生依赖。

操作、作业交付流程和能力边界见 [AI_USAGE.md](AI_USAGE.md)。验证使用分压器和 RC 低通基准。13.3–13.5 的教材电路需要逐题核对，电路图图片可在 Multisim 中打开 .ms10 后截图或打印。
