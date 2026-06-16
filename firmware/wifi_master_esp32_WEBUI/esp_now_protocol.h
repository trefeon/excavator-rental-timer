/*
 * ESP-NOW Protocol — Shared Header
 * Used by both Master and Slave for ESP-NOW communication.
 * Keep this file identical in both projects.
 */
#pragma once
#include <stdint.h>

// ESP-NOW channel must match the Master's SoftAP channel
static const uint8_t ESPNOW_CHANNEL = 1;

// Broadcast address for ESP-NOW
static const uint8_t ESPNOW_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ===== Packet Types =====
enum PacketType : uint8_t {
  PKT_REGISTER_REQ   = 1,  // Slave → Master: "I'm here, assign me an ID"
  PKT_REGISTER_RESP  = 2,  // Master → Slave: "Your ID is X"
  PKT_COMMAND         = 3,  // Master → Slave: "Execute this command"
  PKT_COMMAND_RESP    = 4,  // Slave → Master: "Here's my new state after command"
  PKT_HEARTBEAT       = 5,  // Slave → Master: periodic state push
};

// ===== Command Enums =====
enum CmdType : uint8_t {
  CMD_NONE      = 0,
  CMD_ADD_TIME  = 1,
  CMD_PAUSE     = 2,
  CMD_RESUME    = 3,
  CMD_STOP      = 4,
  CMD_REBOOT    = 5,
  CMD_IDENTIFY  = 6,
};

// ===== Response Code Enums =====
enum RespCode : uint8_t {
  RESP_OK              = 0,
  RESP_BAD_STATE       = 1,
  RESP_EXCEEDS_LIMIT   = 2,
  RESP_UNKNOWN_COMMAND = 3,
  RESP_REBOOTING       = 6,
};

// ===== State Enums (matches RentalState on Slave) =====
enum PktState : uint8_t {
  PKT_STATE_LOCKED  = 0,
  PKT_STATE_RUNNING = 1,
  PKT_STATE_PAUSED  = 2,
  PKT_STATE_ENDED   = 4,
  PKT_STATE_FAULT   = 5,
};

// ===== The Packet =====
struct __attribute__((packed)) EspNowPacket {
  uint8_t  type;       // PacketType
  uint8_t  targetId;   // Which slave this is for (0 = broadcast/any)
  uint8_t  senderId;   // Sender's assigned ID (0 if unregistered)
  uint8_t  cmd;        // CmdType (for PKT_COMMAND / PKT_COMMAND_RESP)
  uint8_t  respCode;   // RespCode (for PKT_COMMAND_RESP)
  uint8_t  state;      // PktState
  uint8_t  seq;        // sequence number
  uint8_t  _pad;       // alignment padding
  uint32_t value;      // time in seconds for ADD_TIME, or 0
  uint32_t timeLeft;   // remaining seconds
  uint8_t  mac[6];     // sender MAC (used during registration)
  uint8_t  _pad2[2];   // alignment to 24 bytes total
};
// sizeof(EspNowPacket) == 24 bytes (well within ESP-NOW's 250-byte limit)

// ===== Helper: Convert CmdType to string =====
inline const char* cmdTypeName(uint8_t cmd) {
  switch ((CmdType)cmd) {
    case CMD_ADD_TIME: return "ADD_TIME";
    case CMD_PAUSE:    return "PAUSE";
    case CMD_RESUME:   return "RESUME";
    case CMD_STOP:     return "STOP";
    case CMD_REBOOT:   return "REBOOT";
    case CMD_IDENTIFY: return "IDENTIFY";
    default:           return "UNKNOWN";
  }
}

// ===== Helper: Convert string command to CmdType =====
inline CmdType cmdFromString(const char* s) {
  if (strcmp(s, "ADD_TIME") == 0)  return CMD_ADD_TIME;
  if (strcmp(s, "PAUSE") == 0)     return CMD_PAUSE;
  if (strcmp(s, "RESUME") == 0)    return CMD_RESUME;
  if (strcmp(s, "STOP") == 0)      return CMD_STOP;
  if (strcmp(s, "REBOOT") == 0)    return CMD_REBOOT;
  if (strcmp(s, "IDENTIFY") == 0)  return CMD_IDENTIFY;
  return CMD_NONE;
}

// ===== Helper: Convert RespCode to string =====
inline const char* respCodeName(uint8_t code) {
  switch ((RespCode)code) {
    case RESP_OK:              return "OK";
    case RESP_BAD_STATE:       return "BAD_STATE";
    case RESP_EXCEEDS_LIMIT:   return "EXCEEDS_LIMIT";
    case RESP_UNKNOWN_COMMAND: return "UNKNOWN_COMMAND";
    case RESP_REBOOTING:       return "REBOOTING";
    default:                   return "UNKNOWN";
  }
}

// ===== Helper: Convert PktState to string =====
inline const char* pktStateName(uint8_t s) {
  switch ((PktState)s) {
    case PKT_STATE_LOCKED:  return "LOCKED";
    case PKT_STATE_RUNNING: return "RUNNING";
    case PKT_STATE_PAUSED:  return "PAUSED";
    case PKT_STATE_ENDED:   return "ENDED";
    case PKT_STATE_FAULT:   return "FAULT";
    default:                return "FAULT";
  }
}
