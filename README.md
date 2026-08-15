# 免责声明

本项目仅用于个人学习研究、本地配置管理与自有设备上的可恢复性测试，不代表、隶属于、授权自或受任何游戏及其运营方认可。使用本项目即表示你已阅读、理解并同意自行承担全部使用后果；如不同意，请勿下载、运行、传播或基于本项目进行二次开发。

使用者必须遵守所在地现行有效的法律法规、部门规章、监管要求及司法解释，包括但不限于网络安全、数据安全、个人信息保护、著作权、计算机信息系统安全保护、反不正当竞争、民事责任、行政责任与刑事责任相关规定；同时必须遵守所使用游戏的用户协议、游戏规则、社区规范、安全策略、反作弊规则及其后续更新。若本项目说明、功能或使用方式与法律法规或游戏官方规则存在冲突，应以法律法规和游戏官方规则为准，并立即停止使用。

请勿将本项目用于以下场景：

- 绕过、对抗或破坏游戏安全机制、反作弊系统、风控策略、远程配置、运营策略或正常服务秩序；
- 获取不正当优势、影响游戏公平性、干扰其他玩家体验、自动化作弊、外挂、脚本、封包篡改或类似用途；
- 未经授权修改、分发、逆向、复制、抓取或利用游戏及第三方的客户端、服务端、数据、素材、接口、账号或商业内容；
- 传播违法违规内容、侵犯他人合法权益，或以任何方式规避法律责任、平台规则、账号处罚或安全审查。

本项目会修改游戏渲染流程中的照片数据，可能导致照片异常、功能异常、安全软件拦截、账号风险提示、账号限制、封禁、数据丢失或其他不可预期后果。请仅在明确理解影响范围、已备份原始文件、可接受风险且确认不违反官方规则的前提下使用；不再使用时请及时移除注入并恢复原始文件。

本仓库及其代码、文档、发行物、Issue、讨论区和相关页面可能因合规、版权、平台规则、维护成本或其他原因随时删除、归档、私有化、迁移或停止更新，作者不承诺持续维护、可用性、兼容性、技术支持或历史版本保留。请勿将本仓库作为长期可用来源或唯一备份来源。

<div align="center">
<br />

# 🖼️ GameAvatarSwap

**拍照时把游戏头像照片替换为自定义图片的本地实验工具：生成、热重载、替换，一条命令完成。**

> 让游戏头像不再局限于默认照片——准备一张自定义图，拍照的瞬间自动换成你的图片。

![GameAvatarSwap](https://socialify.git.ci/shiratsukihiyori/GameAvatarSwap/image?description=1&font=KoHo&forks=1&issues=1&language=1&name=1&owner=1&pattern=Circuit%20Board&pulls=1&stargazers=1&theme=Auto)

[![GitHub license](https://img.shields.io/github/license/shiratsukihiyori/GameAvatarSwap?style=flat-square)](https://github.com/shiratsukihiyori/GameAvatarSwap/blob/main/LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/shiratsukihiyori/GameAvatarSwap?style=flat-square)](https://github.com/shiratsukihiyori/GameAvatarSwap/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/shiratsukihiyori/GameAvatarSwap?style=flat-square)](https://github.com/shiratsukihiyori/GameAvatarSwap/network)
[![GitHub issues](https://img.shields.io/github/issues/shiratsukihiyori/GameAvatarSwap?style=flat-square)](https://github.com/shiratsukihiyori/GameAvatarSwap/issues)
[![GitHub pull requests](https://img.shields.io/github/issues-pr/shiratsukihiyori/GameAvatarSwap?style=flat-square)](https://github.com/shiratsukihiyori/GameAvatarSwap/pulls)

</div>

---

## 项目简介

GameAvatarSwap 是一个 Windows 本地实验工具：通过 D3D11 渲染管线的钩子，在游戏执行头像拍照的时间点，把写入照片的数据替换为自定义图片，从而实现“拍照即换头像”的效果。

- 支持 `512 / 256 / 128` 三种照片尺寸，分别配置独立图片
- 图片、配置 2 秒热重载，改完无需重启游戏
- 可选用原版照片提取圆形遮罩与黑边，避免方形图片“露角”
- 提供独立加载器（`loader.exe`），也支持启动器插件方式加载
- 所有运行文件（配置、日志、图片）都放在 DLL 同目录，不写系统目录

> [!IMPORTANT]
> 本项目是实验性工具，仅面向自有设备与个人学习。使用前请确认不违反你所使用游戏的用户协议与运营规则，并自行承担风险。

## 核心能力

<table>
  <thead>
    <tr>
      <th align="center">Swap</th>
      <th align="center">Reload</th>
      <th align="center">Mask</th>
    </tr>
  </thead>
  <tr>
    <td width="33%">
      <strong>🖼️ 拍照替换</strong><br />
      <sub>在照片写入的时间点替换数据，三种尺寸独立开关与图片。</sub>
    </td>
    <td width="33%">
      <strong>♻️ 热重载</strong><br />
      <sub>配置与图片每 2 秒自动重载，换图、改开关都不用重启。</sub>
    </td>
    <td width="33%">
      <strong>⭕ 圆形遮罩</strong><br />
      <sub>可从原版照片提取圆形 alpha 并置黑圆外区域，避免白角外露。</sub>
    </td>
  </tr>
</table>

## 工作原理

游戏头像照片通常由 D3D11 渲染管线生成：先渲染到离屏纹理，再经过复制、映射、编码、上传等步骤。本工具使用 [MinHook](https://github.com/TsudaKageyu/minhook) 对 `ID3D11DeviceContext` 的相关调用进行钩子拦截：

- 在照片离屏纹理被复制/映射读取的时机，把目标尺寸的纹理内容替换为自定义图片
- 可选：在计算着色器读取照片前注入（覆盖更完整的渲染路径）
- 替换完成后，游戏后续的编码与上传流程保持不变，得到的就是自定义图片

> 本项目不修改游戏文件、不修改网络请求、不触碰账号相关数据，仅在本地渲染流程中替换照片内容。

## 快速开始

### 环境要求

- Windows 10/11 x64
- 支持 D3D11 的游戏（目标进程）
- 构建需要 [MSYS2 MinGW-w64](https://www.msys2.org/)（gcc/g++/ar），仅使用不构建则不需要
- Python 3.8+（仅生成图片时需要，含 `Pillow`、`numpy`）

### 1. 克隆并构建

```powershell
git clone https://github.com/shiratsukihiyori/GameAvatarSwap.git
cd GameAvatarSwap
.\src\build.ps1        # 产物输出到 build\
```

### 2. 生成替换图片

```powershell
python tools\prep_img.py --src 你的头像.jpg
# 可选：用一张游戏原版照片提取圆形遮罩
python tools\prep_img.py --src 你的头像.jpg --mask 原版照片.png
```

会生成 `custom_512.png / custom_256.png / custom_128.png`，放到 DLL 同目录即可。

### 3. 放置运行文件

把以下文件放到同一目录（例如 `build\`）：

```
AvatarHook.dll
hook_config.txt        # 复制 config\hook_config.txt.example 并改名
custom_512.png
custom_256.png
custom_128.png
```

按需修改 `hook_config.txt`（2 秒后自动生效）。

### 4. 加载

两种方式任选其一：

- **启动器插件**：如果你的启动器支持插件注入，按 `config\plugin_config.ini.example` 的格式登记 `AvatarHook.dll`，启用后随游戏启动自动加载；
- **独立加载器**（游戏先启动或 loader 先启动都可以；30 秒内未通过窗口钩子注入会自动改用远程线程兜底）：

```powershell
.\build\loader.exe .\build\AvatarHook.dll 目标进程名.exe
```

> 注入成功后请保持 loader 窗口开着，关闭窗口会卸载钩子。

### 5. 验证

拍照一次，检查照片是否已变成自定义图片。状态文件 `hook_loaded.txt` 会记录加载结果：

| 内容 | 含义 |
| --- | --- |
| `textures=3 flip=0` | 配置与 3 张图片加载成功 |
| `HOOKED ...` | 钩子已启用 |
| `image_load_failed` | 图片不存在或尺寸不符（需 512/256/128 正方形） |
| `no_config` | 未找到 `hook_config.txt` |

## 配置说明

`hook_config.txt` 全部键值（示例见 `config/hook_config.txt.example`）：

| 键 | 说明 |
| --- | --- |
| `FLIP` | `1` = 图片上下翻转（照片坐标补偿） |
| `REPLACE` | 总开关（兼容项） |
| `REPLACE_512/256/128` | 各尺寸替换开关 |
| `512/256/128` | 对应尺寸图片路径（相对路径基于 DLL 目录） |
| `CSINJECT` | 计算着色器注入（主流程，建议 `1`） |
| `RTINJECT` / `SRVINJECT` | 其他注入路径（高级，默认 `0`） |
| `COPYLOG` / `FULLLOG` | 日志开关 |
| `TARGET` | 可选：仅对指定进程名生效，留空不限制 |

## 工具脚本

- `tools/prep_img.py` — 生成替换图片：居中裁剪正方形 → 缩放 → 可选翻转 → 可选圆形遮罩。用法见脚本头部注释。
- `tools/watch_photos.py` — 可选辅助：监视游戏照片目录，把本地预览文件也替换为自定义图片（原始文件自动备份为 `*.watcherbak`）。注入本身已生效时此脚本非必需。

## 目录结构

```
GameAvatarSwap/
├── src/
│   ├── hook_dll.cpp            # 主钩子 DLL（D3D11 替换逻辑）
│   ├── loader.cpp              # 独立加载器（CBT 钩子 / 远程线程）
│   ├── build.ps1 / build.sh    # 构建脚本
├── config/
│   ├── hook_config.txt.example     # 运行时配置示例
│   └── plugin_config.ini.example   # 启动器插件登记示例
├── tools/
│   ├── prep_img.py             # 生成替换图片
│   └── watch_photos.py         # 照片目录监视辅助
├── third_party/
│   ├── minhook/                # MinHook（MIT，vendored）
│   └── stb/stb_image.h         # stb_image（public domain）
└── build/                      # 构建产物（不入库）
```

## 故障排查

- **`image_load_failed`**：图片不是对应尺寸的正方形（512/256/128），或路径不对。检查图片尺寸与路径。
- **替换没生效**：确认 `REPLACE_*` 与 `CSINJECT` 开关、确认 `hook_loaded.txt` 显示 `HOOKED`。
- **方形白角外露**：用 `prep_img.py --mask 原版照片.png` 重新生成，提取圆形遮罩并置黑圆外。
- **方向反了**：调整 `prep_img.py` 的 `--flip`（`v` / `h` / `none`）重新生成图片即可。
- **杀毒软件报毒**：钩子注入类工具常被杀软误报。本项目完全开源可审计；是否添加白名单请自行判断。

## 🥚 彩蛋

这个仓库从头到尾没有写过"这是给哪个游戏做的"。

不是忘了。是不方便写。

如果你（或你的 AI）已经猜到了——恭喜，你猜得没错。

## 📄 许可证

[GPL-3.0](LICENSE)。MinHook 以 MIT 许可证 vendored 于 `third_party/minhook`，stb_image 为 public domain。
