#include "ff.h"
#include "diskio.h"

#include "board_config.h"
#include "sd_card.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#define CMD0    (0x40u + 0u)
#define CMD1    (0x40u + 1u)
#define CMD8    (0x40u + 8u)
#define CMD16   (0x40u + 16u)
#define CMD12   (0x40u + 12u)
#define CMD17   (0x40u + 17u)
#define CMD18   (0x40u + 18u)
#define CMD24   (0x40u + 24u)
#define CMD55   (0x40u + 55u)
#define CMD58   (0x40u + 58u)
#define ACMD41  (0x40u + 41u)

#define SD_BLOCK_SIZE 512u
#define SD_TOKEN      0xFEu

static bool initialized;
static bool block_addressed;

// Own SPI loops: the SDK's spi_*_blocking() live in SRAM, which is full.
static uint8_t transfer(uint8_t value) {
    spi_hw_t *spi = spi_get_hw(BOARD_SD_SPI);
    spi->dr = value;
    while (!(spi->sr & SPI_SSPSR_RNE_BITS)) {}
    return (uint8_t)spi->dr;
}

// Clock count bytes in (sending 0xFF, or source when given), keeping the
// 8-entry FIFOs busy.
static void transfer_bytes(const uint8_t *source, uint8_t *destination,
                           size_t count) {
    spi_hw_t *spi = spi_get_hw(BOARD_SD_SPI);
    size_t sent = 0, received = 0;
    while (received < count) {
        if (sent < count && sent - received < 8 &&
            (spi->sr & SPI_SSPSR_TNF_BITS)) {
            spi->dr = source ? source[sent] : 0xFF;
            ++sent;
        }
        if (spi->sr & SPI_SSPSR_RNE_BITS) {
            uint8_t value = (uint8_t)spi->dr;
            if (destination) destination[received] = value;
            ++received;
        }
    }
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
    transfer_bytes(packet, NULL, sizeof packet);
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
    transfer_bytes(NULL, destination, SD_BLOCK_SIZE);
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
    sd_status.stage = 1;
    sd_status.last_r1 = response;
    if (response != 1) goto failed;

    response = command(CMD8, 0x1AA, 0x87);
    sd_status.stage = 2;
    sd_status.last_r1 = response;
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
        sd_status.stage = 3;
        sd_status.last_r1 = response;
        if (response != 0) goto failed;
        response = command(CMD58, 0, 0x01);
        sd_status.stage = 4;
        sd_status.last_r1 = response;
        if (response != 0) goto failed;
        uint8_t ocr[4];
        for (unsigned i = 0; i < sizeof ocr; ++i) ocr[i] = transfer(0xFF);
        sd_status.ocr = (uint32_t)ocr[0] << 24 | (uint32_t)ocr[1] << 16 |
                        (uint32_t)ocr[2] << 8 | ocr[3];
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
        sd_status.stage = 3;
        sd_status.last_r1 = response;
        if (response != 0) goto failed;
        block_addressed = false;
    }
    if (!block_addressed && command(CMD16, SD_BLOCK_SIZE, 0x01) != 0) goto failed;

    deselect();
    spi_set_baudrate(BOARD_SD_SPI, BOARD_SD_SPI_BAUD);
    initialized = true;
    sd_status.stage = 5;
    return 0;

failed:
    sd_status.fail_stage = sd_status.stage ? sd_status.stage : 1;
    deselect();
    initialized = false;
    return STA_NOINIT;
}

DSTATUS disk_status(BYTE drive) {
    return drive == 0 && initialized ? 0 : STA_NOINIT;
}

// Waits until the card releases MISO (busy after a write or a stop).
static bool wait_ready(uint32_t ms) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    do {
        if (transfer(0xFF) == 0xFF) return true;
    } while (!time_reached(deadline));
    return false;
}

// Ends a multi-block read: CMD12 goes out without reselecting the card,
// a stuff byte follows, then R1 and busy.
static bool stop_transmission(void) {
    const uint8_t packet[6] = { CMD12, 0, 0, 0, 0, 0x01 };
    transfer_bytes(packet, NULL, sizeof packet);
    transfer(0xFF);
    uint8_t response = 0xFF;
    for (unsigned i = 0; i < 10 && (response & 0x80u); ++i)
        response = transfer(0xFF);
    return !(response & 0x80u) && wait_ready(250);
}

bool sd_read_blocks(uint32_t sector, uint8_t *buffer, unsigned count) {
    if (!initialized || !buffer || !count) return false;
    const uint32_t address = block_addressed ? sector : sector * SD_BLOCK_SIZE;
    bool ok;
    if (count == 1) {
        ok = command(CMD17, address, 0x01) == 0 && receive_block(buffer);
    } else {
        ok = command(CMD18, address, 0x01) == 0;
        for (unsigned i = 0; ok && i < count; ++i)
            ok = receive_block(buffer + i * SD_BLOCK_SIZE);
        ok = stop_transmission() && ok;
    }
    deselect();
    return ok;
}

bool sd_write_block(uint32_t sector, const uint8_t *buffer) {
    if (!initialized || !buffer) return false;
    const uint32_t address = block_addressed ? sector : sector * SD_BLOCK_SIZE;
    bool ok = command(CMD24, address, 0x01) == 0;
    if (ok) {
        transfer(0xFF);
        transfer(SD_TOKEN);
        transfer_bytes(buffer, NULL, SD_BLOCK_SIZE);
        transfer(0xFF);                 // CRC, ignored in SPI mode
        transfer(0xFF);
        ok = (transfer(0xFF) & 0x1Fu) == 0x05u;     // data accepted
        ok = wait_ready(500) && ok;
    }
    deselect();
    return ok;
}

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count) {
    if (drive != 0 || !initialized) return RES_NOTRDY;
    if (!buffer || !count) return RES_PARERR;
    return sd_read_blocks((uint32_t)sector, buffer, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count) {
    if (drive != 0 || !initialized) return RES_NOTRDY;
    if (!buffer || !count) return RES_PARERR;
    for (UINT i = 0; i < count; ++i)
        if (!sd_write_block((uint32_t)sector + i, buffer + i * SD_BLOCK_SIZE))
            return RES_ERROR;
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
