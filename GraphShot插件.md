# GraphShot

> Github仓库：[github.com/EanoJiang/GraphShot](https://github.com/EanoJiang/GraphShot)

> 一个 Unreal Engine 编辑器插件，可将任意图编辑器（Graph Editor）的完整截图（包括所有节点、含屏幕外区域）捕获到剪贴板。
>
> Capture complete screenshots of any graph editor (all nodes, including off-screen) to the clipboard.

---

## 引擎版本

- **UE 5.8+**（依赖 `ControlRig` 与 `RigVM` 插件，这些是引擎内置插件，确保在项目中启用即可）

## 安装

### 方式一：源码（推荐，可二次开发）

1. 将本仓库克隆到项目的 `Plugins/GraphShot` 目录下：
   ```bash
   git clone https://github.com/<你的用户名>/GraphShot.git <你的项目>/Plugins/GraphShot
   ```
2. 右键 `.uproject` → **Generate Visual Studio project files**（或在编辑器打开时按提示重新编译）。
3. 在编辑器中打开项目，插件即自动加载。

### 方式二：预编译二进制（跳过编译）

从 [Releases](../../releases) 下载 `GraphShot_<版本>_UE<版本>_win64.zip`，将 `GraphShot` 文件夹解压到项目的 `Plugins/` 目录下即可。

> 预编译包仅适用于 **Windows 64-bit + 对应 UE 版本**，不同引擎版本间不兼容。

## 用法

安装后，在任意图编辑器（动画蓝图、Control Rig、蓝图等）中：

- **快捷键：`Ctrl+Alt+Shift+S`**（默认组合键，可在 *Editor Preferences → Keyboard Shortcuts* 中搜索 `GraphShot` 自行更改）
- 或在**级别编辑器的 `Window` 菜单**里点击 **`GraphShot: Screenshot Current Graph`**（该命令也以按钮形式出现在图编辑器共享工具条上）
  - ![1786524595325](https://img2024.cnblogs.com/blog/3614909/202608/3614909-20260814155351852-1230536850.png)
- 截图将自动写入剪贴板，可直接粘贴（`Ctrl+V`）到画图、文档或聊天工具中。

### 🎯 选区截图（v1.1+）

当你在图编辑器中选中了部分节点/注释时，**仅捕获选中区域**，其余节点自动隐藏，不干扰截图内容。

- 未选中任何节点 → 完整截图（与 v1.0 行为一致）
- 选中节点/注释 → 仅截取选中区域，自动计算边界 + 适当的 padding
- 注释（Comment）也参与选区：选中即纳入，不选中则隐藏

## 依赖

- `ControlRig`（引擎内置插件）
- `RigVM`（引擎内置插件）

确保项目的 `.uproject` 中启用了这两个插件：

```json
{
  "Plugins": [
    { "Name": "ControlRig", "Enabled": true },
    { "Name": "RigVM", "Enabled": true }
  ]
}
```

## 构建

```bash
# 从源码构建（需先生成项目文件）
<引擎目录>/Engine/Build/BatchFiles/RunUAT.bat BuildPlugin -Plugin="<路径>/GraphShot/GraphShot.uplugin" -Package="<输出目录>"
```

## 许可

许可证待定。发布前请先在此指定许可协议（例如 MIT、Apache-2.0 或 GPL-3.0），并在仓库根放置对应的 `LICENSE` 文件。

---

*Created by Eano*
