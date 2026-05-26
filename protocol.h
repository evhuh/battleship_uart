// =============================================================================
// protocol.h — battleship inter-board packet framer
// =============================================================================
// Wire format:
//   START(0xAA) | CMD | LEN | PAYLOAD[LEN] | CRC8 | END(0x55)
// Stop-and-wait reliability. CRC-8 over [CMD, LEN, PAYLOAD] poly 0x07.
//
// IMPORTANT: This module auto-ACKs HELLO, READY, BEGIN, and RESET on receive
// before invoking the application callback. The application does NOT need to
// call protocol_send_ack from those handlers.
// =============================================================================

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <Arduino.h>

const uint8_t PKT_START   = 0xAA;
const uint8_t PKT_END     = 0x55;
const uint8_t MAX_PAYLOAD = 4;

enum Cmd : uint8_t {
  CMD_HELLO  = 0x01,
  CMD_ACK    = 0x02,
  CMD_READY  = 0x03,
  CMD_FIRE   = 0x04,
  CMD_RESULT = 0x05,
  CMD_RESET  = 0x06,
  CMD_NACK   = 0x07,
  CMD_BEGIN  = 0x08,    // "I pressed A on the instructions screen"
};

enum Outcome : uint8_t {
  OUTCOME_MISS         = 0x00,
  OUTCOME_HIT          = 0x01,
  OUTCOME_HIT_SUNK     = 0x02,
  OUTCOME_HIT_GAMEOVER = 0x03,
};

enum NackReason : uint8_t {
  NACK_OUT_OF_TURN    = 0x01,
  NACK_INVALID_STATE  = 0x02,
  NACK_RESET_DECLINED = 0x03,
};

// CRC + packet building
uint8_t crc8(const uint8_t* data, uint8_t len);
uint8_t protocol_build_packet(uint8_t cmd, const uint8_t* payload, uint8_t len,
                              uint8_t* out);

// Parser
enum ParseState : uint8_t {
  PS_WAIT_START, PS_READ_CMD, PS_READ_LEN, PS_READ_PAYLOAD, PS_READ_CRC, PS_READ_END,
};

struct Parser {
  ParseState state;
  uint8_t    cmd;
  uint8_t    len;
  uint8_t    payload[MAX_PAYLOAD];
  uint8_t    payloadIdx;
  uint8_t    expectedCrc;
  uint16_t   frameErrors;
  bool       packetReady;
};

void protocol_parser_init(Parser& p);
bool protocol_parse_byte(Parser& p, uint8_t b);

// Stream-bound API
void protocol_init(Stream& link, uint8_t board_id);
void protocol_poll();

bool protocol_send_hello();
bool protocol_send_ready();
bool protocol_send_begin();
bool protocol_send_fire(uint8_t r, uint8_t c, uint8_t* outOutcome);
bool protocol_send_reset();

bool protocol_send_result(uint8_t r, uint8_t c, uint8_t outcome);
bool protocol_send_ack(uint8_t cmd_acked);
bool protocol_send_nack(uint8_t reason);

// Callbacks — defined in main sketch
extern void onHello(uint8_t their_board_id);
extern void onReady();
extern void onBegin();
extern void onFire(uint8_t r, uint8_t c);
extern void onResult(uint8_t r, uint8_t c, uint8_t outcome);
extern void onReset();
extern void onAck(uint8_t cmd_acked);
extern void onNack(uint8_t reason);
extern void onFrameError();

#endif
