# 局域网打印修复：自研发送器（绕开 Bambu 网络插件）

本补丁解决 **OrcaSlicer-ImageMap 汉化版 v1.0.44 无法通过局域网发送打印任务** 的问题：

> 点击「发送打印任务」后报错 `Failed to upload file to ftp. Please try again.`，
> 或进程直接闪退（`ACCESS_VIOLATION`）。

修复后：**在 app 里点按钮即可直接打印**，完全不需要闭源 Bambu 网络插件。

实测环境：Bambu Lab P1S（`p1sc_xzz`，序列号 `01P00C5C1602733`）局域网模式。

---

## 一、根因分析（三层问题叠加）

### 第 1 层：固件要求 MQTT 命令签名 ← 决定性原因

打印机固件对带 `print` 对象的 MQTT 命令做 **RSA-SHA256 签名校验**。未签名命令会被拒绝并报：

```
HMS 0500-0500-0001-0007 = "MQTT Command verification failed"
```

闭源的 `bambu_networking_*.dll` 插件无法满足该校验，因此**插件发不出打印任务是必然结果**，与插件版本无关。参考
[ClusterM/open-bamboo-networking §10.4 MQTT message signing](https://github.com/ClusterM/open-bamboo-networking/blob/master/research/10.04-mqtt-signing.md)：

> When **Developer Mode** is off, every MQTT payload with a top-level `"print"` object must carry a verified `header` envelope. […] Firmware rejects **unsigned** privileged commands when Developer Mode is off. **Developer Mode makes the `header` envelope optional.**

**解决方式：在打印机上开启「开发者模式」**（设置 → 通用/网络 → LAN 模式附近）。
开启后未签名命令被放行，本补丁的实现即可工作。

> 本补丁**不提取、不使用** Bambu Studio 的私有签名密钥 —— 那属于绕过厂商访问控制。

### 第 2 层：插件在 `ft_tunnel_sync_connect` 处崩溃

`FileTransferTunnel` 构造函数在插件加载失败时留下 `h_ = nullptr`，而 `sync_start_connect()`
未经校验就把该空句柄传给插件，插件内部随即解引用非法指针 → `ACCESS_VIOLATION` 闪退。
此外 `ft_abi_version()` 虽被查询但从未校验，18 处 `m_->ft_*` 调用均无保护。

本补丁做了**全面加固**（`02-network-plugin-abi-hardening.patch`）：句柄校验、ABI 版本校验、
所有插件调用点加保护 —— 即使插件不可用也只是返回错误码，绝不崩溃。

### 第 3 层：存储异常与目录缺失

排查中还遇到两类打印失败，均已处理：

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `HMS 0500-C010` 存储读写异常 | 存储被延时摄影占满（实测 `/timelapse` 占 3.66 GB） | 清理存储；见第 4 节 |
| `550 Server denied you to change to the given directory` | 格式化存储卡后 `/cache` 目录消失 | 代码自动 `MKD /cache` |

---

## 二、修复方案：自研 LAN 发送器

新增 **`src/slic3r/Utils/BambuLanSender.{hpp,cpp}`**，用程序自带的 libcurl + OpenSSL
实现与官方客户端等价的完整三步流程，**不引入任何新依赖**：

| 步骤 | 协议 | 实现 |
| --- | --- | --- |
| 1 | **FTPS 990**（隐式 TLS，`bblp` + 访问码） | libcurl 上传切片文件到 `/cache`，同时写 `<名字>.gcode.3mf` 与裸 `<名字>.3mf` |
| 2 | 同上 | 写伴生文件 `1_<名字>.gcode.bbl`（任务元数据 JSON） |
| 3 | **MQTT 8883**（TLS） | 自写精简 MQTT 3.1.1 客户端（OpenSSL）发布 `project_file` 到 `device/<序列号>/request` |

接入点在 `src/slic3r/GUI/Jobs/PrintJob.cpp`：在插件分支**之前**插入自研路径，
成功后直接跳到收尾流程，**完全不调用插件**（因此不会再闪退）。

### 关键实现细节（踩过的坑）

1. **`url` 字段必须用 `file:///sdcard/cache/<名字>.gcode.3mf`**
   P1/A1 系列与 X1 系列不同，不能用文档示例里的 `ftp:///`。
2. **`sequence_id` 必须单调递增** —— 固定 `"0"` 会被固件拒绝。
3. **必须上传到 `/cache/` 目录**（`ftps://host:990/cache/<name>`），
   而非 FTP 根目录，否则打印机按 `file:///sdcard/cache/...` 找不到文件。
4. **格式化存储卡后 `/cache` 会消失**，需先 `MKD /cache`。
5. **任务名需为 ASCII 且每次唯一**，代码自动清洗项目名并追加时间戳。
6. **MQTT 报文解析**：读取「剩余长度」varint 字节后必须把它们**拼回报文**，
   否则 CONNACK 会少一个字节而被判失败。
7. **TLS 读取不能用 `select()` 把关** —— TLS 记录可能已解密进 OpenSSL 内部缓冲区，
   此时 `select()` 在裸 socket 上永远不触发。应依赖 `SO_RCVTIMEO`。

### 行为开关

| 配置项 | 默认 | 说明 |
| --- | --- | --- |
| `use_builtin_lan_sender` | 开启 | 设为 `"false"` 可回退旧的插件流程 |
| `skip_lan_print_preflight` | 跳过 | 是否执行插件隧道的发送前探测 |
| `warn_newer_3mf_version` | 关闭 | 是否提示「3MF 版本较新」 |

配置文件位置：`%APPDATA%\OrcaSlicer-ImageMap\OrcaSlicer.conf`

### 日志排查关键词

```
print_job: builtin_lan_sender enabled=true, cfg="", connection_type=lan, print_type=from_normal
print_job: builtin LAN send, ip=…, dev_id=…, plate=…, ams=[0,1,2,3], bed_type=textured_plate, name=…, file=…
print_job: builtin LAN send OK, remote=…gcode.3mf
print_job: builtin LAN send FAILED: <原因> (ftp_code=…)
```

日志位置：`%APPDATA%\OrcaSlicer-ImageMap\log\`

---

## 三、补丁文件说明与使用

补丁基于 **[sentientstardust-dev/OrcaSlicer-ImageMap](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap)
的 `v1.0.44` 标签（commit `235e94db`）**。

| 文件 | 内容 |
| --- | --- |
| `00-lan-sender-complete.patch` | **推荐**：自研发送器完整补丁（含新增文件 + PrintJob 接入 + CMake 登记） |
| `01-lan-sender-integration.patch` | 仅 `PrintJob.cpp` 与 `CMakeLists.txt` 的改动 |
| `02-network-plugin-abi-hardening.patch` | 插件 ABI / 空句柄加固 |
| `03-gui-suppressions.patch` | 抑制有害的插件降级提示 + 3MF 版本告警 |

### 应用方式

```bash
# 1) 取得上游源码并切到同一基线
git clone https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap.git
cd OrcaSlicer-ImageMap
git checkout v1.0.44

# 2) 应用完整补丁
git apply /path/to/patches/lan-print/00-lan-sender-complete.patch

# 3) 编译（Windows / MSVC）
#    先准备依赖与构建目录，详见上游 BUILD 文档
cmake --build . --config Release --target OrcaSlicer -- /m:3 /p:CL_MPCount=3
```

**依赖说明**：无需新增。`libcurl` 与 `OpenSSL` 在本项目 CMake 中本来就已链接
（`CMakeLists.txt` 的 `find_package(OpenSSL REQUIRED)` 与 `libcurl` INTERFACE 目标，
`CrealityPrint.cpp`、`Http.cpp` 已在用）。

---

## 四、打印机侧前置条件（重要）

本补丁要让打印真正成功，打印机需满足：

1. **开启开发者模式**（见第 1 节）—— 否则未签名的 MQTT 命令仍会被固件拒绝。
2. **存储空间充足** —— P1S 内置存储约 8 GB，`/timelapse`（延时摄影）与 `/cache`
   （历史切片）最容易占满。建议定期清理：
   - 存储异常（`0500-C010`）时，可完全关机 30 秒后重新插紧存储卡；
     若反复出现，需更换存储卡。
3. **AMS 耗材与任务匹配** —— `ams_mapping` 必须与实际装载的槽位一致，
   否则会打废或报错。本补丁直接沿用界面上的映射结果。

---

## 五、已知限制

- **发送过程约需 1 分钟**（实测 3.4 MB 切片耗时 59 秒），期间界面停在 10% 不动。
  原因是同步上传 + FTPS/MQTT 两次 TLS 握手；**并非闪退**。
  后续可改为异步上传并显示真实进度。
- 仅针对 **P1 系列**做过实测（Bambu Lab P1S）。
  X1 / A1 系列理论上通用（同为 `file:///sdcard` 写法），但未验证。
- 开启开发者模式属于放宽打印机自身的安全校验，请自行评估。

---

## 六、实测记录

```
发送前：gcode_state=FINISH/IDLE, print_error=0
发送：  app 内点击「发送打印任务」
        → print_job: builtin LAN send OK, remote=job_1791145981.gcode.3mf   (耗时 59 秒)
打印机：gcode_state=RUNNING, layer=0/405, nozzle=250.9°C→250, bed=73.2°C→74
        print_error=0
文件：  /cache/job_1791145981.gcode.3mf                3,394,467 B
        /cache/job_1791145981.3mf                      3,394,467 B
        /cache/1_job_1791145981.gcode.bbl                    424 B
        /cache/job_1791145981_plate_1.gcode           12,946,999 B  ← 打印机自行解出
```

---

## 七、许可

补丁内容遵循本仓库 `LICENSE.txt`。所修改的上游源码版权归 OrcaSlicer / Bambu Lab 及各原作者所有。
