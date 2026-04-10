#pragma once

// ============================================================
//  CameraConfig.h  (rvc_app module)
//  All tuneable settings for the rear-view camera service.
//  Edit this file and rebuild — nothing else needs changing.
// ============================================================

namespace rearview {

// ── Camera ───────────────────────────────────────────────────────────────────
static constexpr const char* CAMERA_ID       = "100";
static constexpr int         VIDEO_WIDTH     = 1280;
static constexpr int         VIDEO_HEIGHT    = 720;
static constexpr int         CAPTURE_FPS     = 30;

// ── H.264 Encoding ───────────────────────────────────────────────────────────
static constexpr const char* MIME_TYPE       = "video/avc";
static constexpr int         VIDEO_BITRATE   = 2'000'000;   // 2 Mbps
static constexpr int         I_FRAME_INTERVAL = 1;          // IDR every 1 s

// ── RTP / Network ────────────────────────────────────────────────────────────
static constexpr const char* RTP_DEST_IP     = "192.168.10.10";
static constexpr int         RTP_DEST_PORT   = 5004;
static constexpr const char* RTP_LOCAL_IFACE = "eth0";      // SO_BINDTODEVICE forces eth0 egress
static constexpr int         RTP_LOCAL_PORT  = 0;           // OS-assigned

// RTP payload type for H.264 (RFC 6184)
static constexpr uint8_t     RTP_PAYLOAD_TYPE = 96;

// Maximum RTP packet payload — keep below network MTU (1500) minus IP+UDP headers
static constexpr int         RTP_MTU          = 1400;

} // namespace rearview
