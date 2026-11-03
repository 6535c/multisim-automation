# 给 AI 的 Multisim 10 自动化操作说明

## 输入与核对

先读取题图或教材原页，逐项提取元件型号、阻容值、电源、接地、引脚、激励参数和测量位置。第 13.3、13.4、13.5 节的作业清单只给出了题名和提交物，实际电路仍需对应教材页。请把三道题各自放在独立目录，避免覆盖用户原图。

## 环境

使用 Windows、Multisim 10.0.1.0 x86、注册好的 32 位 Multisim COM 组件和 Python 3.10 或更新版本。首次运行前在 PowerShell 中设置 `$env:MULTISIM_HOME` 指向 Multisim 安装目录。默认路径沿用开发机配置。`native-build.json` 对原生 DLL 和运行时做 SHA-256 校验。包内附带桥接程序。要重编译时使用 x86 Visual Studio Developer Command Prompt 并运行 `build_native.cmd`。

## 从电路到交付

1. 按 `examples/voltage_divider.json` 或 `examples/rc_lowpass.json` 写操作配方。用 `catalog` 检索库名，放置后用 `inspect` 核对 `terminal`、节点和实际数值。用独立目录保存每题的 `.ms10`。
2. 用 `python multisim.py run recipe.json --output exercise.ms10` 生成真实 Multisim 图纸，关闭并重开检查。若作业要电路图图片，在 Multisim 中打开该文件，调整缩放后截图或打印到 PDF。
3. 分别执行下面三类分析。选用题目规定的时长和频率区间。工作点从 `result.json` 提取关键节点电压和支路电流。

```powershell
python multisim.py solve exercise.ms10 --analysis op --output-dir op
python multisim.py solve exercise.ms10 --analysis transient --duration 0.001 --output-dir transient
python multisim.py solve exercise.ms10 --analysis ac --start-frequency 1000 --stop-frequency 10000000 --points-per-decade 30 --output-dir ac
```

4. 从各输出目录的 `result.json` 取真实向量名。可用 `schematic.json` 或 Python 的 `SimulationResult.voltage(ref, pin)` 把节点映射到元件端子。下面的 `$outTrace` 和 `$inTrace` 应设置为实际向量名。含 `$` 的向量名请用 PowerShell 单引号录入变量。

```powershell
python plot_result.py transient/result.json $outTrace transient.svg
python plot_result.py ac/result.json $outTrace ac.svg --reference $inTrace
```

AC 图使用对数频率轴。指定输入参考后绘制 `20 log10(|Vout/Vin|)`。瞬态图使用求解器时间点。SVG 可直接在浏览器中查看或插入支持 SVG 的报告。若题目要求相位，用 AC 复数向量另行计算相角。第 13.5 节应给出振荡器启动和稳态波形，检查仿真时长、模型和题目允许的启动扰动。

## 验收

- 13.3、13.4 各交付原理图、DC 工作点表、瞬态图和 AC 扫频图。13.5 交付原理图与瞬态图。
- 检查 `circuit.cir` 中器件模型引用是否存在，检查直流偏置、波形幅度与频率，以及 AC 激励。高频晶体管、谐振负载和自启动振荡器按具体电路核验。
- 留存原生 `.ms10`、`circuit.cir`、`result.json` 和 SVG。说明测量端、单位、分析设置和误差。用另一进程重开图纸核对。
- 保存前处理所有错误。若编辑批次失败，关闭该会话，从最后一个可重开的文件开始。总线主干可放置，成员线连接及复杂多页图纸仍属研究范围。
