# ESP32 网关固件升级与回滚

OTA 用于智能家居的 ESP32 网关。STM32 负责传感器、OLED、风扇和窗帘，ESP32 的 USB 迁移和后续 OTA 均在网关侧进行。网关重启后恢复 Wi-Fi、MQTT 和 UART 会话。

## 首次安装

1. 按[整机接线](上板配置与验收.md#智能家居联网)确认网关串口为 GPIO25 TX、GPIO16 RX，ThingsBoard 凭据留在本机 `ESP32/sdkconfig`。在 **Edge monitoring terminal** 启用 **USB-authorized firmware updates from the local network**，ThingsBoard 配置默认开启。
2. 连接 ESP32 USB，关闭串口监视器，确认 ESP32 型号和串口后备份原 Flash。首次安装需要更新引导程序和分区表。`partitions.csv` 保留 NVS、PHY 和第一个应用区的起始地址，两个应用槽各 1.5 MiB。完整备份留在本机。
3. 用以下命令生成基线。适用于 4 MiB Flash 的经典 ESP32。

```powershell
.\Tools\Build-ESP32.ps1 -Version 1.0.0 -OutputDirectory ESP32/build-artifacts/ota-baseline
```

4. 在基线产物目录按 `flash_args` 烧录引导程序、分区表、初始化的 OTA 数据和应用。打开串口查询状态。首次启动需要先确认 UART 任务、网关网络栈与 HTTP 服务已初始化，以及剩余堆至少为 24 KiB，然后会登记第一个 OTA 槽并自动重启一次。看到 `image_state=valid`、`healthy=true` 后再做网络升级。

```powershell
python -m pip install pyserial
python Tools/ota_gateway.py --port <网关串口> status
```

## 局域网升级

电脑与网关连接同一可信局域网。通过 `status` 获得网关 IPv4，在浏览器打开 `http://<网关 IPv4>/`。每次升级使用新的版本号，上传独立的 `edge_gateway.bin` 应用文件。

```powershell
.\Tools\Build-ESP32.ps1 -Version 1.0.1 -OutputDirectory ESP32/build-artifacts/ota-next
python Tools/ota_gateway.py --port <网关串口> arm
```

把终端的一次性口令填入网页。授权有效期为 300 秒，成功授权的上传请求开始后口令立即作废。网关核对项目名和 OTA 兼容标记，在备用槽分块写入，比较 SHA-256 并校验镜像格式，之后才设置启动分区。网页会等待新版确认或显示回滚结果。也可用命令行上传，工具会先核对 USB 与 LAN 上的网关身份和运行版本。

```powershell
python Tools/ota_gateway.py --port <网关串口> upload ESP32/build-artifacts/ota-next/edge_gateway.bin
```

网页服务使用局域网 HTTP 和物理 USB 授权，部署时保持路由器端口转发关闭。SHA-256 检查上传内容的完整性。生产环境的固件来源认证应采用镜像签名与安全启动方案。

## 整机验收

1. 保持基线运行，用下一版本的文件分别执行 `upload <应用文件> --bad-digest` 和 `upload <应用文件> --abort-after 16384`。确认校验不符和中断都保留了基线，再正常上传同一个文件，记录升级前后的有效版本与应用槽。
2. 确认正常新版已经成为有效回退版本，再编译单独的故障验证应用。它只用于触发启动检查失败。

```powershell
.\Tools\Build-ESP32.ps1 -Version 1.0.2-check -DiagnosticRollback -OutputDirectory ESP32/build-artifacts/ota-rollback
python Tools/ota_gateway.py --port <网关串口> upload ESP32/build-artifacts/ota-rollback/edge_gateway.bin --expect-rollback
```

3. 在网关重启和回滚期间观察 STM32 OLED、传感器和执行器。回滚后在手机面板验证遥测更新与手动控制回执。启动确认的条件采用 ESP32 本地检查，整机联网和执行器验证单独记录。

实际的升级、回滚与重连结果见[整机验收记录](软件验证记录.md#esp32-网关-ota-整机验收2026-11-26)。
