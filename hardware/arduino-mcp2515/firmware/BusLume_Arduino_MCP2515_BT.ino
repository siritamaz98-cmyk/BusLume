#include <can.h>
#include <mcp2515.h>

#include <CanHacker.h>
#include <CanHackerLineReader.h>
#include <lib.h>

#include <SPI.h>
#include <SoftwareSerial.h>

// BusLume Arduino + MCP2515 prototype
// MCP2515 module: 8 MHz oscillator
// SPI: CS=D10, INT=D2, MOSI=D11, MISO=D12, SCK=D13
// HC-05: TXD -> D3, RXD <- D4 through a voltage divider

const byte SPI_CS_PIN = 10;
const byte INT_PIN    = 2;

const byte BT_RX_PIN = 3;  // Arduino RX  <- HC-05 TXD
const byte BT_TX_PIN = 4;  // Arduino TX  -> HC-05 RXD through divider

const unsigned long USB_SPEED = 115200;
const unsigned long BT_SPEED  = 115200;

SoftwareSerial bluetooth(BT_RX_PIN, BT_TX_PIN);

CanHackerLineReader *lineReader = nullptr;
CanHacker *canHacker = nullptr;

void setup() {
    Serial.begin(USB_SPEED);
    bluetooth.begin(BT_SPEED);

    SPI.begin();
    pinMode(INT_PIN, INPUT);

    // BusLume communicates over HC-05.
    // USB Serial is kept as the debug stream for bench diagnostics.
    Stream *interfaceStream = &bluetooth;
    Stream *debugStream = &Serial;

    canHacker = new CanHacker(interfaceStream, debugStream, SPI_CS_PIN);
    lineReader = new CanHackerLineReader(canHacker);
}

void loop() {
    // Process commands arriving from BusLume / HC-05.
    lineReader->process();

    // MCP2515 INT is active LOW when a CAN event is waiting.
    if (digitalRead(INT_PIN) == LOW) {
        canHacker->processInterrupt();
    }
}
