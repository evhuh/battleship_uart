// =============================================================================
// protocol.cpp — battleship inter-board packet framer
// =============================================================================

#include "protocol.h"

// ===========================================================================
// CRC-8 (polynomial 0x07, init 0x00)
// ===========================================================================
uint8_t crc8(const uint8_t* data, uint8_t len) {
  uint8_t crc = 0;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; i++)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
  }
  return crc;
}

// ===========================================================================
// Packet building
// ===========================================================================
uint8_t protocol_build_packet(uint8_t cmd, const uint8_t* payload, uint8_t len,
                              uint8_t* out) {
  out[0] = PKT_START;
  out[1] = cmd;
  out[2] = len;
  for (uint8_t i = 0; i < len; i++) out[3 + i] = payload[i];

  uint8_t crcBuf[2 + MAX_PAYLOAD];
  crcBuf[0] = cmd;
  crcBuf[1] = len;
  for (uint8_t i = 0; i < len; i++) crcBuf[2 + i] = payload[i];
  out[3 + len] = crc8(crcBuf, 2 + len);
  out[4 + len] = PKT_END;
  return 5 + len;
}

// ===========================================================================
// Parser
// ===========================================================================
void protocol_parser_init(Parser& p) {
  p.state        = PS_WAIT_START;
  p.cmd          = 0;
  p.len          = 0;
  p.payloadIdx   = 0;
  p.expectedCrc  = 0;
  p.frameErrors  = 0;
  p.packetReady  = false;
}

static void parserResetToWait(Parser& p) {
  p.state      = PS_WAIT_START;
  p.payloadIdx = 0;
}

bool protocol_parse_byte(Parser& p, uint8_t b) {
  if (p.packetReady) return false;

  switch (p.state) {
    case PS_WAIT_START:
      if (b == PKT_START) p.state = PS_READ_CMD;
      break;

    case PS_READ_CMD:
      p.cmd = b;
      p.state = PS_READ_LEN;
      break;

    case PS_READ_LEN:
      if (b > MAX_PAYLOAD) {
        p.frameErrors++;
        parserResetToWait(p);
        break;
      }
      p.len = b;
      p.payloadIdx = 0;
      p.state = (p.len == 0) ? PS_READ_CRC : PS_READ_PAYLOAD;
      break;

    case PS_READ_PAYLOAD:
      p.payload[p.payloadIdx++] = b;
      if (p.payloadIdx >= p.len) {
        p.state = PS_READ_CRC;
      }
      break;

    case PS_READ_CRC: {
      uint8_t crcBuf[2 + MAX_PAYLOAD];
      crcBuf[0] = p.cmd;
      crcBuf[1] = p.len;
      for (uint8_t i = 0; i < p.len; i++) crcBuf[2 + i] = p.payload[i];
      uint8_t computed = crc8(crcBuf, 2 + p.len);
      if (computed != b) {
        p.frameErrors++;
        parserResetToWait(p);
      } else {
        p.state = PS_READ_END;
      }
      break;
    }

    case PS_READ_END:
      if (b == PKT_END) {
        p.packetReady = true;
        p.state = PS_WAIT_START;
        return true;
      } else {
        p.frameErrors++;
        parserResetToWait(p);
      }
      break;
  }
  return false;
}

// ===========================================================================
// Stream-bound state
// ===========================================================================
static Stream*  g_link    = nullptr;
static uint8_t  g_boardId = 0;
static Parser   g_parser;

static volatile uint8_t g_awaiting       = 0;
static volatile bool    g_replyReceived  = false;
static volatile uint8_t g_replyOutcome   = 0;
static volatile uint8_t g_fireR = 0, g_fireC = 0;

void protocol_init(Stream& link, uint8_t board_id) {
  g_link = &link;
  g_boardId = board_id;
  protocol_parser_init(g_parser);
  g_awaiting = 0;
  g_replyReceived = false;
}

static void sendBytes(const uint8_t* buf, uint8_t n) {
  if (!g_link) return;
  for (uint8_t i = 0; i < n; i++) g_link->write(buf[i]);
}

static bool sendPacket(uint8_t cmd, const uint8_t* payload, uint8_t len) {
  uint8_t buf[5 + MAX_PAYLOAD];
  uint8_t n = protocol_build_packet(cmd, payload, len, buf);
  sendBytes(buf, n);
  return true;
}

// Auto-ACK helper: send an ACK with the given cmd back.
static void autoAck(uint8_t cmd_acked) {
  uint8_t payload = cmd_acked;
  sendPacket(CMD_ACK, &payload, 1);
}

static void dispatchPacket(const Parser& p) {
  // ---- Auto-ACK first, BEFORE dispatching to application -----------------
  // This way the ACK goes out promptly, even if the application's callback
  // does some heavier work (display redraws, etc).
  switch (p.cmd) {
    case CMD_HELLO:
    case CMD_READY:
    case CMD_BEGIN:
    case CMD_RESET:
      autoAck(p.cmd);
      break;
    default:
      break;
  }

  // ---- Application callbacks ---------------------------------------------
  switch (p.cmd) {
    case CMD_HELLO:
      onHello(p.len ? p.payload[0] : 0);
      break;
    case CMD_READY:
      onReady();
      break;
    case CMD_BEGIN:
      onBegin();
      break;
    case CMD_FIRE:
      if (p.len >= 2) onFire(p.payload[0], p.payload[1]);
      break;
    case CMD_RESULT:
      if (p.len >= 3) onResult(p.payload[0], p.payload[1], p.payload[2]);
      break;
    case CMD_RESET:
      onReset();
      break;
    case CMD_ACK:
      onAck(p.len ? p.payload[0] : 0);
      break;
    case CMD_NACK:
      onNack(p.len ? p.payload[0] : 0);
      break;
  }

  // ---- Reply matching for blocking sends ---------------------------------
  if (g_awaiting == 0 || g_replyReceived) return;

  if (g_awaiting == CMD_ACK && p.cmd == CMD_ACK) {
    g_replyReceived = true;
  } else if (g_awaiting == CMD_RESULT && p.cmd == CMD_RESULT && p.len >= 3) {
    if (p.payload[0] == g_fireR && p.payload[1] == g_fireC) {
      g_replyOutcome  = p.payload[2];
      g_replyReceived = true;
    }
  }
}

void protocol_poll() {
  if (!g_link) return;
  while (g_link->available() > 0) {
    int b = g_link->read();
    if (b < 0) break;
    if (protocol_parse_byte(g_parser, (uint8_t)b)) {
      dispatchPacket(g_parser);
      g_parser.packetReady = false;
    }
  }
}

static bool waitForReply(uint8_t cmd, uint16_t timeoutMs) {
  g_awaiting = cmd;
  g_replyReceived = false;
  uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    protocol_poll();
    if (g_replyReceived) {
      g_awaiting = 0;
      return true;
    }
  }
  g_awaiting = 0;
  return false;
}

static bool sendBlocking(uint8_t cmd, const uint8_t* payload, uint8_t len,
                         uint8_t expectedReply, uint16_t timeoutMs,
                         uint8_t tries) {
  for (uint8_t attempt = 0; attempt < tries; attempt++) {
    sendPacket(cmd, payload, len);
    if (waitForReply(expectedReply, timeoutMs)) return true;
  }
  return false;
}

// ===========================================================================
// Public send functions
// ===========================================================================
bool protocol_send_hello() {
  uint8_t payload = g_boardId;
  return sendBlocking(CMD_HELLO, &payload, 1, CMD_ACK, 2000, 3);
}

bool protocol_send_ready() {
  return sendBlocking(CMD_READY, nullptr, 0, CMD_ACK, 5000, 3);
}

bool protocol_send_begin() {
  return sendBlocking(CMD_BEGIN, nullptr, 0, CMD_ACK, 30000, 1);
  // Long timeout, no retry: the partner might be reading the instructions.
  // Single attempt: we don't want to spam BEGINs.
}

bool protocol_send_fire(uint8_t r, uint8_t c, uint8_t* outOutcome) {
  uint8_t payload[2] = {r, c};
  g_fireR = r;
  g_fireC = c;
  bool ok = sendBlocking(CMD_FIRE, payload, 2, CMD_RESULT, 3000, 3);
  if (ok && outOutcome) *outOutcome = g_replyOutcome;
  return ok;
}

bool protocol_send_reset() {
  return sendBlocking(CMD_RESET, nullptr, 0, CMD_ACK, 2000, 2);
}

bool protocol_send_result(uint8_t r, uint8_t c, uint8_t outcome) {
  uint8_t payload[3] = {r, c, outcome};
  return sendPacket(CMD_RESULT, payload, 3);
}

bool protocol_send_ack(uint8_t cmd_acked) {
  return sendPacket(CMD_ACK, &cmd_acked, 1);
}

bool protocol_send_nack(uint8_t reason) {
  return sendPacket(CMD_NACK, &reason, 1);
}
