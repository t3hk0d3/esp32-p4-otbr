#pragma once

// Fixed hardware facts of the Waveshare ESP32-P4-WIFI6-POE-ETH (spec 2.1, 2.2).

// Ethernet: IP101GRI on the internal EMAC, RMII, 50 MHz reference clock input from the PHY.
// The RMII data pins are the P4 defaults (TX_EN 49, TXD0 34, TXD1 35, CRS_DV 28, RXD0 29, RXD1 30).
#define P4_PIN_ETH_MDC 31
#define P4_PIN_ETH_MDIO 52
#define P4_PIN_ETH_PHY_RST 51
#define P4_PIN_ETH_RMII_CLK 50
#define P4_ETH_PHY_ADDR 1

// RCP link to the ESP32-C6 (SPI2, mode 0).
#define P4_PIN_RCP_SCLK 18
#define P4_PIN_RCP_MOSI 19
#define P4_PIN_RCP_MISO 14
#define P4_PIN_RCP_CS 15
#define P4_PIN_RCP_INT 16
#define P4_RCP_SPI_HZ 10000000

// C6 control: UPDATE low at reset boots the updater; RESET drives C6 CHIP_PU.
#define P4_PIN_C6_UPDATE 6
#define P4_PIN_C6_RESET 54

// Optional maintenance jumper to the C6 header H7.
#define P4_PIN_JUMPER_RX 4
#define P4_PIN_JUMPER_TX 5
#define P4_PIN_JUMPER_BOOT 22
