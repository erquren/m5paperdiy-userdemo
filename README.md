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
