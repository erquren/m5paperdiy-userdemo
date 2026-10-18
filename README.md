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
