# OrcaSlicer-ImageMap 汉化版 v1.0.44

本版本把上游 **OrcaSlicer-ImageMap v1.0.44**（Windows 便携版）重新打包，并**新增中文界面翻译**。

## 下载哪个

| 文件 | 说明 |
| --- | --- |
| `OrcaSlicer-ImageMap-CN-v1.0.44-portable.zip`（约 175 MB） | **完整便携包**：程序本体 + 中文补丁 + 使用说明，解压即用 |
| `汉化-仅补丁-v1.0.44.zip`（几百 KB） | **只含中文补丁**：适合已经有上游便携版的用户，自己运行补丁脚本 |

## 完整包用法

1. 右键 zip → **属性 → 解除锁定** → 解压到任意目录（路径尽量简单）。
2. 双击 **`启动 OrcaSlicer-ImageMap.cmd`**：首次运行会自动清除 Internet 标记、
   在桌面创建快捷方式，并启动应用。
3. **不要用"以管理员身份运行"**。

## 本次改动（可复核）

- 仅修改 `OrcaSlicer/resources/i18n/zh_CN/OrcaSlicer.mo`：条目 **5219 → 5425**，**新增 206 条**中文，
  未覆盖或删除任何既有翻译；程序本体未改动。
- 对应源码（AGPL 意义上的 preferred form）随包提供：`汉化素材/zh-tsv.tsv`、`汉化素材/apply-zh-mo.py`。
- 覆盖：管理颜色数据对话框、纹理映射区菜单/按钮、调制模式与选项、2D/线性/径向渐变、
  顶点色与 RGBA 操作、图像纹理导入/载入/投影、擦拭塔图像、ImageMap 新增设置项的标签与提示。

## 已知限制

- 云功能（登录 / 云打印 / 云预设同步 / MakerWorld / 远程监控）在上游服务端已不可用；
  本地切片、ImageMap、LAN 模式与 SD 卡打印正常。
- 用更新版 BambuStudio 保存的 3MF 会提示版本兼容警告（点确认即可使用）。

## 来源与许可

上游：<https://gitlab.com/sentient_stardust/orcaslicer-imagemap> ·
<https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap>（AGPL-3.0，见 `LICENSE.txt`）。
本包与 OrcaSlicer / Bambu Lab 官方无任何关联；完整包内含 Bambu / Agora 等非 AGPL 的第三方组件，
版权归各自所有者。如对再分发有疑虑，请只使用补丁包。
