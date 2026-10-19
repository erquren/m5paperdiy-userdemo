/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <clock/clock_power.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <fcntl.h>
#include <sd_protocol_defs.h>
#include <unistd.h>

namespace {

constexpr char APP_TAG[]               = "HalTfCard";
constexpr char MOUNT_POINT[]           = "/sdcard";
constexpr gpio_num_t PIN_MISO          = GPIO_NUM_40;
constexpr gpio_num_t PIN_MOSI          = GPIO_NUM_38;
constexpr gpio_num_t PIN_SCLK          = GPIO_NUM_39;
constexpr gpio_num_t PIN_CS            = GPIO_NUM_47;
constexpr gpio_num_t PIN_DET           = GPIO_NUM_48;
constexpr std::size_t TEST_SIZE_BYTES  = 1024 * 1024;
constexpr std::size_t TEST_BUFFER_SIZE = 16 * 1024;

std::string cardType(const sdmmc_card_t& card)
{
    if (card.is_sdio) {
        return "SDIO";
    }
    if (card.is_mmc) {
        return "MMC";
    }
    return (card.ocr & SD_OCR_SDHC_CAP) != 0 ? "SDHC/SDXC" : "SDSC";
}

std::string cardBrand(const sdmmc_card_t& card)
{
    char name[sizeof(card.cid.name) + 1] = {};
    std::memcpy(name, card.cid.name, sizeof(card.cid.name));
    return name[0] == '\0' ? "UNKNOWN" : name;
}

bool writeAll(int fd, const std::uint8_t* buffer, std::size_t size)
{
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = write(fd, buffer + written, size - written);
        if (result <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

bool readAndVerify(int fd, std::uint8_t* buffer, std::size_t size)
{
    std::size_t received = 0;
    while (received < size) {
        const ssize_t result = read(fd, buffer + received, size - received);
        if (result <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(result);
    }
    for (std::size_t i = 0; i < size; ++i) {
        if (buffer[i] != static_cast<std::uint8_t>((i * 31U + 17U) & 0xFFU)) {
            return false;
        }
    }
    return true;
}

float throughputMbPerSecond(std::size_t bytes, std::int64_t elapsed_us)
{
    if (elapsed_us <= 0) {
        return 0.0f;
    }
    return static_cast<float>(bytes) * 1000000.0f / (static_cast<float>(elapsed_us) * 1024.0f * 1024.0f);
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                              TF Card Presence                              */
/* -------------------------------------------------------------------------- */
bool Hal::initTfCard()
{
    const gpio_config_t detect_config = {
        .pin_bit_mask = 1ULL << PIN_DET,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&detect_config) == ESP_OK;
}

bool Hal::isTfCardInserted() const
{
    return gpio_get_level(PIN_DET) == 0;
}

const TfCardInfo& Hal::updateTfCard(bool retry_mount)
{
    if (!isTfCardInserted()) {
        unmountTfCard();
        _tf_card_info         = {};
        _tf_card_info.status  = TfCardStatus::NoCard;
        _tf_card_info.message = "NO TF CARD";
        return _tf_card_info;
    }

    if (_tf_card != nullptr) {
        return _tf_card_info;
    }
    if (_tf_card_info.status == TfCardStatus::MountFailed && !retry_mount) {
        return _tf_card_info;
    }

    mountTfCard();
    return _tf_card_info;
}

const TfCardInfo& Hal::getTfCardInfo() const
{
    return _tf_card_info;
}

/* -------------------------------------------------------------------------- */
/*                               TF Card Mount                                */
/* -------------------------------------------------------------------------- */
bool Hal::mountTfCard()
{
    _tf_card_info = {};

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    if (!_tf_spi_initialized) {
        const spi_bus_config_t bus_config = {
            .mosi_io_num           = PIN_MOSI,
            .miso_io_num           = PIN_MISO,
            .sclk_io_num           = PIN_SCLK,
            .quadwp_io_num         = GPIO_NUM_NC,
            .quadhd_io_num         = GPIO_NUM_NC,
            .data4_io_num          = GPIO_NUM_NC,
            .data5_io_num          = GPIO_NUM_NC,
            .data6_io_num          = GPIO_NUM_NC,
            .data7_io_num          = GPIO_NUM_NC,
            .data_io_default_level = false,
            .max_transfer_sz       = TEST_BUFFER_SIZE,
            .flags                 = 0,
            .isr_cpu_id            = ESP_INTR_CPU_AFFINITY_AUTO,
            .intr_flags            = 0,
        };
        const esp_err_t error =
            spi_bus_initialize(static_cast<spi_host_device_t>(host.slot), &bus_config, SDSPI_DEFAULT_DMA);
        if (error != ESP_OK) {
            _tf_card_info.status  = TfCardStatus::MountFailed;
            _tf_card_info.message = esp_err_to_name(error);
            ESP_LOGE(APP_TAG, "SPI initialization failed: %s", esp_err_to_name(error));
            return false;
        }
        _tf_spi_initialized = true;
    }

    gpio_set_pull_mode(PIN_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_CS, GPIO_PULLUP_ONLY);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed           = false;
    mount_config.max_files                        = 4;
    mount_config.allocation_unit_size             = 16 * 1024;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = PIN_CS;
    slot_config.host_id               = static_cast<spi_host_device_t>(host.slot);

    const esp_err_t error = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &_tf_card);
    if (error != ESP_OK) {
        _tf_card              = nullptr;
        _tf_card_info.status  = TfCardStatus::MountFailed;
        _tf_card_info.message = esp_err_to_name(error);
        ESP_LOGE(APP_TAG, "TF mount failed: %s", esp_err_to_name(error));
        return false;
    }

    _tf_card_info.status         = TfCardStatus::Mounted;
    _tf_card_info.type           = cardType(*_tf_card);
    _tf_card_info.brand          = cardBrand(*_tf_card);
    _tf_card_info.capacity_bytes = static_cast<std::uint64_t>(_tf_card->csd.capacity) * _tf_card->csd.sector_size;
    _tf_card_info.message        = "READY";
    ESP_LOGI(APP_TAG, "TF mounted: %s, %s, %llu bytes", _tf_card_info.type.c_str(), _tf_card_info.brand.c_str(),
             static_cast<unsigned long long>(_tf_card_info.capacity_bytes));
    return true;
}

void Hal::unmountTfCard()
{
    if (_tf_card == nullptr) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, _tf_card);
    _tf_card = nullptr;
    ESP_LOGI(APP_TAG, "TF unmounted");
}

/* -------------------------------------------------------------------------- */
/*                              TF Card Benchmark                             */
/* -------------------------------------------------------------------------- */
TfBenchmarkResult Hal::benchmarkTfCard()
{
    // The benchmark can outlive the gesture that enters clock mode.
    ClockPowerGuard power;
    TfBenchmarkResult result;
    if (_tf_card == nullptr || _tf_card_info.status != TfCardStatus::Mounted) {
        result.message = "CARD NOT READY";
        return result;
    }

    std::uint8_t* buffer = static_cast<std::uint8_t*>(heap_caps_malloc(TEST_BUFFER_SIZE, MALLOC_CAP_DMA));
    if (buffer == nullptr) {
        result.message = "NO MEMORY";
        return result;
    }
    for (std::size_t i = 0; i < TEST_BUFFER_SIZE; ++i) {
        buffer[i] = static_cast<std::uint8_t>((i * 31U + 17U) & 0xFFU);
    }

    // Long file names are disabled, so use a unique 8.3 name without replacing user data.
    char path[32] = {};
    int fd        = -1;
    for (unsigned int attempt = 0; attempt < 4 && fd < 0; ++attempt) {
        const unsigned long suffix = (static_cast<unsigned long>(millis()) + attempt) & 0xFFFFFUL;
        std::snprintf(path, sizeof(path), "%s/PDT%05lX.BIN", MOUNT_POINT, suffix);
        fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    }
    if (fd < 0) {
        const int open_errno = errno;
        result.message       = "OPEN FAILED";
        ESP_LOGE(APP_TAG, "Benchmark file open failed: errno=%d (%s)", open_errno, std::strerror(open_errno));
        heap_caps_free(buffer);
        return result;
    }

    const std::int64_t write_start = esp_timer_get_time();
    bool io_ok                     = true;
    for (std::size_t offset = 0; offset < TEST_SIZE_BYTES; offset += TEST_BUFFER_SIZE) {
        if (!writeAll(fd, buffer, TEST_BUFFER_SIZE)) {
            io_ok = false;
            break;
        }
    }
    if (io_ok && fsync(fd) != 0) {
        io_ok = false;
    }
    const std::int64_t write_elapsed = esp_timer_get_time() - write_start;
    close(fd);

    if (!io_ok) {
        result.message = "WRITE FAILED";
        unlink(path);
        heap_caps_free(buffer);
        return result;
    }

    const int read_fd = open(path, O_RDONLY);
    if (read_fd < 0) {
        result.message = "READ OPEN FAILED";
        unlink(path);
        heap_caps_free(buffer);
        return result;
    }

    const std::int64_t read_start = esp_timer_get_time();
    for (std::size_t offset = 0; offset < TEST_SIZE_BYTES; offset += TEST_BUFFER_SIZE) {
        if (!readAndVerify(read_fd, buffer, TEST_BUFFER_SIZE)) {
            io_ok = false;
            break;
        }
    }
    const std::int64_t read_elapsed = esp_timer_get_time() - read_start;
    close(read_fd);
    unlink(path);
    heap_caps_free(buffer);

    if (!io_ok) {
        result.message = "READ VERIFY FAILED";
        return result;
    }

    result.success    = true;
    result.write_mb_s = throughputMbPerSecond(TEST_SIZE_BYTES, write_elapsed);
    result.read_mb_s  = throughputMbPerSecond(TEST_SIZE_BYTES, read_elapsed);
    result.message    = "OK";
    return result;
}
