# PaperDIY User Demo

### SKU: K159, K159-B

## Related Link

- [K159 Document & Datasheet](https://docs.m5stack.com/en/products/sku/K159).
- [K159-B Document & Datasheet](https://docs.m5stack.com/en/products/sku/K159-B).

PaperDIY firmware is developed with ESP-IDF for the K159 and K159-B SKUs.

## Demos

- Battery voltage monitoring
- Wi-Fi network scanning
- TF card information and read/write test
- A/B button feedback
- A+B long-press shutdown

## IDF Build

### Tool Chains

[ESP-IDF v5.5.1](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/index.html)

### Fetch Components

Components are pinned in `repos.json` and project patches are applied automatically.

```bash
python fetch_repos.py
```

### Build

```bash
idf.py build
```

### Flash

```bash
idf.py flash
```

### Monitor

```bash
idf.py monitor
```

## Project Layout

- `main/apps/` - Firmware applications.
- `main/hal/` - Board hardware abstraction.
- `main/assets/` - Embedded display images.
- `components/` - M5Stack and third-party components.
- `patches/` - Project-specific component patches.

## Acknowledgments

This project references the following open-source libraries and resources:

- https://github.com/m5stack/M5GFX
- https://github.com/m5stack/M5Unified
- https://github.com/Forairaaaaa/mooncake

## License

- [MIT](LICENSE)

## 手机网页配网（本地功能分支）

本功能面向上述 PaperDIY K159 / K159-B（ESP32-S3），支持 2.4 GHz Wi-Fi。

1. 单独长按 A 约 3 秒打开临时设置热点。
2. 扫描屏幕上的 Wi-Fi 二维码，或输入屏幕显示的热点名称和随机密码。
3. 手机若自动弹出设置页，直接使用；否则打开 `http://192.168.4.1`。
   手机提示“无互联网”时，选择继续连接该热点。页面不依赖互联网或 CDN。
4. 选择附近网络或手动输入隐藏 SSID，输入密码并连接。
5. 设备取得路由器分配的 IP 且配置保存成功后显示成功，并关闭设置热点。
   取得 IP 不等于已验证互联网可访问。

正常页面的 A 短按仍用于扫描，B 短按仍用于 TF 卡测试，A+B 长按仍为关机。
配网页内 B 为退出；A+B 会先退出配网页，再进入原关机流程。
设置热点使用每次生成的 WPA2 密码，最多允许一个客户端；5 分钟无配网操作后关闭。
状态轮询不会延长超时。连接成功后约 10 秒关闭热点。

### 行为与限制

- 配置只在成功获得 IP 后保存到独立 NVS namespace；错误密码或连接失败不会覆盖旧配置。
- 下一次启动自动尝试已保存网络。认证失败、网络不存在和 DHCP 超时有边界，不会无间隔无限重试。
- 配网服务只处理热点接口请求；写入操作验证随机会话令牌。不会打印 Wi-Fi 密码。
- 当前固件未开启 flash/NVS 加密。持久化密码不具备防物理读取保证；此功能不会修改 eFuse 或安全设置。
- AP+STA 共用无线信道，尝试连接路由器时手机可能短暂断开。页面会重试读取状态，设备屏幕也会显示结果。
- 自动弹窗依赖手机系统；始终保留手动地址。不能重定向 HTTPS 网站。
- 该实现不是联网路由器，没有开启 NAT 或为手机转发互联网流量。
- 配网页使用 PSRAM 暂存原屏幕；退出时恢复。内存不足时不会开启热点。
- 首次空白 NVS 启动仍保留原项目的“显示出厂 logo 并关机”行为，重新开机后使用正常功能。
- 不会在 NVS 初始化错误时自动清空整个 NVS 分区，以保留已保存配置和出厂标记。

### 验证范围

主机逻辑测试和完整交叉编译不能替代实机验证。烧录前确认具体 SKU，并备份所需原固件。
建议实机验收：iOS/Android 自动弹窗和手动地址、错误密码、隐藏 SSID、2.4 GHz 网络、
断开/重连、超时退出、保存后掉电重启、换网失败后旧配置、连续多次进入退出、
配网期间 A+B 关机，以及 TF 卡和电池区域恢复。

## Erquren Clock（本地功能分支）

- 连续三次短按 B 切换全屏时钟；再三击 B 返回进入前的页面。
  每次短按不超过 400 ms，相邻松开间隔不超过 400 ms，三次松开的总跨度不超过 800 ms。
  三击不会触发 TF 测试；紧随其后的快速多余点击会被吸收。
- 时钟是 960×540 横屏白底：左上角 24 px 中文日期和星期，中央 256 px HH:MM，
  右下角 20 px `Erquren Clock`，边距 32 px。日期月/日与确认的预览一致，不补零。
- 时间固定为中国标准时间 UTC+8。只有成功接受网络校时后才显示真实日期/时间；
  尚未校时时中央显示 `--:--`，日期留空。断网后继续计时，重启后需要重新取得可信时间。
- 按真实整分钟边界更新；跨日或校时向前/向后跳到另一分钟时立即更新。
  单纯网络状态变化不会重刷时钟。每 40 次局部刷新由原 HAL 做一次清屏刷新。
- 时钟期间电池、TF、扫描状态和按钮边框均不覆盖画面；短按 A/B 不触发后台操作。
  长按 A 会先退出时钟，再执行原有配网页进入/退出操作。A+B 会先恢复原页面和方向，
  再进入原有长按关机流程；取消关机后停留在恢复后的页面。
- 从配网页进入时钟会关闭热点。退出时钟会恢复普通页面，不会自动重开旧热点；
  时钟内长按 A 会退出时钟并主动打开新的配网页。
- 进入时使用 PSRAM 暂存原页面，退出时恢复原方向并释放暂存。
  暂存内存不足时留在当前页面，不消耗内部 Wi-Fi 堆。

字体直接来自已确认预览的 DejaVu Sans / Noto Sans CJK 子集，以原始尺寸生成 16 级灰度位图。
来源、完整许可证、校验值和可重复生成说明见 `main/assets/clock-fonts/README.md`。
固件和主机截图使用同一套布局、字形数据及绘制代码；浏览器与面板的边缘像素和波形仍可能不同。

普通模式取得有效 Wi-Fi IP 后立即尝试 SNTP 校时，成功后每 24 小时再校时。Wi-Fi 连接不代表互联网可达；失败使用有限重试和退避。时间源依次为阿里云、腾讯、pool.ntp.org，普通 SNTP 不提供认证安全时间。

M5PM1 关机会切断 ESP32 供电，重新开机需重新校时；墨水屏断电保留的画面不会继续更新。此模式尚未做实物功耗、无线网络或屏幕残影测试，不承诺具体续航时长。

## 时钟模式 Wi-Fi 节电

- 进入时钟后关闭配网热点，停止 Wi-Fi 驱动；禁止后台扫描和常驻自动重连。有效时间仍离线走时。
- 没有可信时间时，或者距最近一次成功校时满 24 小时，使用已保存的网络开启一次最多 90 秒的连接/校时窗口。这个上限包括旧热点关闭、连接、DHCP、DNS 和 SNTP。
- 只有收到并接受有效 SNTP 回复才算成功，随后立即关闭 Wi-Fi；取得 IP 本身不算校时完成。
- 窗口失败后关闭 Wi-Fi，至少等待一小时才再次尝试；退出再进入时钟不会清除失败/取消窗口的退避。没有已保存的网络时，不自动打开热点或扫描，请长按 A 主动配网。
- 三击 B 退出时钟恢复普通界面的原有联网行为。长按 A 退出并主动配网属于用户覆盖，不受时钟的每日联网限制。
- 首次校时前仍显示 --:--，校时失败但已有有效时间时继续走时。关机后仍需重新校时。

## 时钟 CPU 浅睡眠与按键唤醒

- 时钟空闲且屏幕刷新完成、无线网络完全关闭后，主任务等待到下一整分钟、校时期限或按键通知；FreeRTOS tickless idle 配合 ESP-IDF 自动 light sleep 保留 RAM、时间锚点和页面。
- GPIO4/A、GPIO3/B 使用低电平唤醒。首个按键唤醒后恢复 2 ms 快速采样，保持完整消抖、三击、长 A、A+B 和释放确认；全部动作结束后才恢复中断等待。普通模式保持原有轮询行为。
- 时钟空闲允许 CPU 在 40–240 MHz 间按需调整。普通模式、按键处理、无线网络任务、屏幕完整刷新波形和 TF 测试受到满频/禁止浅睡眠保护。
- 时钟隐藏的按钮边框任务阻塞等待退出，不再每 10 ms 检查；停用未被应用使用的 PMIC 软件电源按键轮询，不改变 PMIC 硬件电源键功能。
- 显示驱动原本已在完整波形结束后关闭升压电路；本版等待波形完成才释放电源管理保护，不直接切断独立 EPD_PWR 电源轨。
- USB Serial/JTAG 或调试器连接时保持唤醒，方便日志和调试。判断电池节电效果应拔掉 USB。SDK/RTOS 自身的必要定时器仍可能引起短暂唤醒。
- 电源管理或按键中断初始化失败时回退为常醒轮询，不冒险进入无法按键唤醒的长等待。没有采用 deep sleep，也没有更改 Flash 加密、eFuse 或启动安全设置。

续航需要电池侧电流与实机测试，不能仅凭编译和主机模拟测试承诺时长。
