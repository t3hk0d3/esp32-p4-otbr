#pragma once

/* C6 pins shared by c6_rcp and c6_updater (Waveshare ESP32-P4-WIFI6-POE-ETH on-board C6). */
#define C6_PIN_SCLK   19
#define C6_PIN_MOSI   18
#define C6_PIN_MISO   20
#define C6_PIN_CS     21
#define C6_PIN_INT    22
#define C6_PIN_UPDATE 2

/* Size of each C6 application slot (ota_0, ota_1) in partitions_c6.csv; the largest RCP image the updater accepts. */
#define C6_OTA_SLOT_SIZE 0x180000

/* C6 flash regions the P4 touches during ROM recovery (partitions_c6.csv, spec 4.1). */
#define C6_FLASH_BOOTLOADER_OFFSET 0x0      /* second-stage bootloader; ends where the partition table starts */
#define C6_FLASH_PTABLE_OFFSET     0x8000   /* partition table; ends where nvs starts */
#define C6_FLASH_NVS_OFFSET        0x9000
#define C6_FLASH_OTADATA_OFFSET    0xf000
#define C6_FLASH_OTADATA_SIZE      0x2000
#define C6_FLASH_TEST_OFFSET       0x20000  /* test partition: the frozen updater */
#define C6_FLASH_TEST_SIZE         0x80000
#define C6_FLASH_OTA0_OFFSET       0xa0000  /* first RCP slot: ROM recovery never touches it or anything after it */
