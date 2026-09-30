#include "ff.h"
#include "diskio.h"

#include "board_config.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#define CMD0    (0x40u + 0u)
#define CMD1    (0x40u + 1u)
#define CMD8    (0x40u + 8u)
#define CMD16   (0x40u + 16u)
#define CMD17   (0x40u + 17u)
#define CMD55   (0x40u + 55u)
#define CMD58   (0x40u + 58u)
#define ACMD41  (0x40u + 41u)

#define SD_BLOCK_SIZE 512u
#define SD_TOKEN      0xFEu

static bool initialized;
static bool block_addressed;

static uint8_t transfer(uint8_t value) {
    uint8_t received;
    spi_write_read_blocking(BOARD_SD_SPI, &value, &received, 1);
    return received;
}

static void deselect(void) {
    gpio_put(BOARD_SD_CS_PIN, 1);
    transfer(0xFF);
}

static void select(void) {
    gpio_put(BOARD_SD_CS_PIN, 0);
    transfer(0xFF);
}

static uint8_t command(uint8_t cmd, uint32_t argument, uint8_t crc) {
    uint8_t packet[6] = {
        cmd, (uint8_t)(argument >> 24), (uint8_t)(argument >> 16),
        (uint8_t)(argument >> 8), (uint8_t)argument, crc
    };
    deselect();
    select();
    spi_write_blocking(BOARD_SD_SPI, packet, sizeof packet);
    for (unsigned i = 0; i < 10; ++i) {
        uint8_t response = transfer(0xFF);
        if (!(response & 0x80u)) return response;
    }
    return 0xFF;
}

static bool receive_block(uint8_t *destination) {
    absolute_time_t deadline = make_timeout_time_ms(250);
    uint8_t token;
    do {
        token = transfer(0xFF);
        if (token != 0xFF) break;
    } while (!time_reached(deadline));
    if (token != SD_TOKEN) return false;
    spi_read_blocking(BOARD_SD_SPI, 0xFF, destination, SD_BLOCK_SIZE);
    transfer(0xFF);
    transfer(0xFF);
    return true;
}

DSTATUS disk_initialize(BYTE drive) {
    if (drive != 0) return STA_NOINIT;

    spi_init(BOARD_SD_SPI, 400000u);
    gpio_set_function(BOARD_SD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(BOARD_SD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(BOARD_SD_MISO_PIN, GPIO_FUNC_SPI);
    gpio_init(BOARD_SD_CS_PIN);
    gpio_set_dir(BOARD_SD_CS_PIN, GPIO_OUT);
    gpio_put(BOARD_SD_CS_PIN, 1);
    gpio_pull_up(BOARD_SD_MISO_PIN);
    for (unsigned i = 0; i < 10; ++i) transfer(0xFF);

    uint8_t response = 0xFF;
    for (unsigned i = 0; i < 1000 && response != 1; ++i) {
        response = command(CMD0, 0, 0x95);
        sleep_ms(1);
    }
    if (response != 1) goto failed;

    response = command(CMD8, 0x1AA, 0x87);
    if (response == 1) {
        uint8_t r7[4];
        for (unsigned i = 0; i < sizeof r7; ++i) r7[i] = transfer(0xFF);
        if (r7[2] != 0x01 || r7[3] != 0xAA) goto failed;
        for (unsigned i = 0; i < 2000; ++i) {
            command(CMD55, 0, 0x01);
            response = command(ACMD41, 1u << 30, 0x01);
            if (response == 0) break;
            sleep_ms(1);
        }
        if (response != 0 || command(CMD58, 0, 0x01) != 0) goto failed;
        uint8_t ocr[4];
        for (unsigned i = 0; i < sizeof ocr; ++i) ocr[i] = transfer(0xFF);
        block_addressed = (ocr[0] & 0x40u) != 0;
    } else {
        for (unsigned i = 0; i < 2000; ++i) {
            command(CMD55, 0, 0x01);
            response = command(ACMD41, 0, 0x01);
            if (response == 0) break;
            response = command(CMD1, 0, 0x01);
            if (response == 0) break;
            sleep_ms(1);
        }
        if (response != 0) goto failed;
        block_addressed = false;
    }
    if (!block_addressed && command(CMD16, SD_BLOCK_SIZE, 0x01) != 0) goto failed;

    deselect();
    spi_set_baudrate(BOARD_SD_SPI, BOARD_SD_SPI_BAUD);
    initialized = true;
    return 0;

failed:
    deselect();
    initialized = false;
    return STA_NOINIT;
}

DSTATUS disk_status(BYTE drive) {
    return drive == 0 && initialized ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count) {
    if (drive != 0 || !initialized) return RES_NOTRDY;
    if (!buffer || !count) return RES_PARERR;
    for (UINT i = 0; i < count; ++i) {
        uint32_t address = (uint32_t)(sector + i);
        if (!block_addressed) address *= SD_BLOCK_SIZE;
        if (command(CMD17, address, 0x01) != 0 ||
            !receive_block(buffer + i * SD_BLOCK_SIZE)) {
            deselect();
            return RES_ERROR;
        }
        deselect();
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE drive, BYTE command_code, void *buffer) {
    if (drive != 0 || !initialized) return RES_NOTRDY;
    if (command_code == CTRL_SYNC) return RES_OK;
    if (command_code == GET_SECTOR_SIZE && buffer) {
        *(WORD *)buffer = SD_BLOCK_SIZE;
        return RES_OK;
    }
    if (command_code == GET_BLOCK_SIZE && buffer) {
        *(DWORD *)buffer = 1;
        return RES_OK;
    }
    return RES_PARERR;
}
