#include "RtpStreamer.h"
#include "CameraConfig.h"

#include <log/log.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <algorithm>

namespace rearview {

static constexpr int RTP_HEADER_SIZE = 12;
static constexpr int FUA_HEADER_SIZE = 2;
// 90 kHz RTP clock for video (RFC 3550)
static constexpr int64_t RTP_CLOCK_RATE = 90'000;

// ─────────────────────────────────────────────────────────────────────────────

RtpStreamer::RtpStreamer() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    mSsrc            = static_cast<uint32_t>(std::rand());
    mSeqNum          = static_cast<uint16_t>(std::rand());
    mTimestampOffset = static_cast<uint32_t>(std::rand());
}

RtpStreamer::~RtpStreamer() {
    stop();
}

bool RtpStreamer::start() {
    if (mRunning.load()) return true;

    mSocket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (mSocket < 0) {
        ALOGE("RtpStreamer: failed to create UDP socket: %s", strerror(errno));
        return false;
    }

    std::memset(&mDestAddr, 0, sizeof(mDestAddr));
    mDestAddr.sin_family = AF_INET;
    mDestAddr.sin_port   = htons(static_cast<uint16_t>(RTP_DEST_PORT));
    if (::inet_pton(AF_INET, RTP_DEST_IP, &mDestAddr.sin_addr) != 1) {
        ALOGE("RtpStreamer: invalid destination IP: %s", RTP_DEST_IP);
        ::close(mSocket);
        mSocket = -1;
        return false;
    }

    mRunning.store(true);
    ALOGI("RtpStreamer: started → %s:%d", RTP_DEST_IP, RTP_DEST_PORT);
    return true;
}

void RtpStreamer::stop() {
    if (!mRunning.exchange(false)) return;
    if (mSocket >= 0) {
        ::close(mSocket);
        mSocket = -1;
    }
    ALOGI("RtpStreamer: stopped");
}

// ── Public: receive Annex-B data from MediaCodec ──────────────────────────────

void RtpStreamer::sendAnnexB(const uint8_t* data, size_t size, int64_t presentationTimeUs) {
    if (!mRunning.load() || !data || size == 0) return;

    const int64_t rtpTs = toRtpTimestamp(presentationTimeUs);

    // Walk the Annex-B stream and dispatch each NAL unit.
    // Start codes: 0x000001 (3-byte) or 0x00000001 (4-byte).
    size_t nalStart = 0;
    bool   inNal    = false;

    auto dispatchNal = [&](size_t end) {
        if (inNal && end > nalStart) {
            sendNalUnit(data + nalStart, end - nalStart, rtpTs);
        }
    };

    size_t i = 0;
    while (i < size) {
        const bool sc3 = (i + 2 < size) &&
                         data[i]==0x00 && data[i+1]==0x00 && data[i+2]==0x01;
        const bool sc4 = (i + 3 < size) &&
                         data[i]==0x00 && data[i+1]==0x00 &&
                         data[i+2]==0x00 && data[i+3]==0x01;

        if (sc3 || sc4) {
            dispatchNal(i);
            nalStart = i + (sc4 ? 4 : 3);
            inNal    = true;
            i        = nalStart;
        } else {
            ++i;
        }
    }
    dispatchNal(size); // flush last NAL
}

// ── Internal NAL dispatch ─────────────────────────────────────────────────────

void RtpStreamer::sendNalUnit(const uint8_t* nal, size_t size, int64_t rtpTimestamp) {
    if (size == 0) return;

    if (static_cast<int>(size) + RTP_HEADER_SIZE <= RTP_MTU) {
        sendSingleNal(nal, size, rtpTimestamp, /*marker=*/true);
    } else {
        sendFuA(nal, size, rtpTimestamp);
    }
}

void RtpStreamer::sendSingleNal(const uint8_t* nal, size_t size,
                                int64_t rtpTimestamp, bool marker) {
    std::vector<uint8_t> packet(RTP_HEADER_SIZE + size);
    writeRtpHeader(packet.data(), marker, rtpTimestamp);
    std::memcpy(packet.data() + RTP_HEADER_SIZE, nal, size);
    sendUdp(packet.data(), packet.size());
}

void RtpStreamer::sendFuA(const uint8_t* nal, size_t size, int64_t rtpTimestamp) {
    const uint8_t fuIndicator = static_cast<uint8_t>((nal[0] & 0xE0) | 28);
    const uint8_t nalType     = static_cast<uint8_t>(nal[0] & 0x1F);

    const int maxPayload = RTP_MTU - RTP_HEADER_SIZE - FUA_HEADER_SIZE;
    size_t    offset     = 1; // skip original NAL header byte
    bool      first      = true;

    while (offset < size) {
        const size_t chunk = std::min(static_cast<size_t>(maxPayload), size - offset);
        const bool   last  = (offset + chunk >= size);

        const uint8_t fuHeader = static_cast<uint8_t>(
            (first ? 0x80 : 0x00) |
            (last  ? 0x40 : 0x00) |
            (nalType & 0x1F));

        std::vector<uint8_t> packet(RTP_HEADER_SIZE + FUA_HEADER_SIZE + chunk);
        writeRtpHeader(packet.data(), /*marker=*/last, rtpTimestamp);
        packet[RTP_HEADER_SIZE]     = fuIndicator;
        packet[RTP_HEADER_SIZE + 1] = fuHeader;
        std::memcpy(packet.data() + RTP_HEADER_SIZE + FUA_HEADER_SIZE,
                    nal + offset, chunk);
        sendUdp(packet.data(), packet.size());

        offset += chunk;
        first   = false;
    }
}

void RtpStreamer::writeRtpHeader(uint8_t* buf, bool marker, int64_t rtpTimestamp) {
    const uint16_t seq = mSeqNum++;
    buf[0] = 0x80; // V=2, P=0, X=0, CC=0
    buf[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (RTP_PAYLOAD_TYPE & 0x7F));
    buf[2] = static_cast<uint8_t>(seq >> 8);
    buf[3] = static_cast<uint8_t>(seq & 0xFF);

    const uint32_t ts = static_cast<uint32_t>(rtpTimestamp);
    buf[4] = static_cast<uint8_t>(ts >> 24);
    buf[5] = static_cast<uint8_t>(ts >> 16);
    buf[6] = static_cast<uint8_t>(ts >>  8);
    buf[7] = static_cast<uint8_t>(ts & 0xFF);

    buf[8]  = static_cast<uint8_t>(mSsrc >> 24);
    buf[9]  = static_cast<uint8_t>(mSsrc >> 16);
    buf[10] = static_cast<uint8_t>(mSsrc >>  8);
    buf[11] = static_cast<uint8_t>(mSsrc & 0xFF);
}

int64_t RtpStreamer::toRtpTimestamp(int64_t presentationTimeUs) const {
    return (mTimestampOffset + presentationTimeUs * RTP_CLOCK_RATE / 1'000'000LL)
           & 0xFFFFFFFFLL;
}

void RtpStreamer::sendUdp(const uint8_t* data, size_t size) {
    if (mSocket < 0) return;
    const ssize_t sent = ::sendto(mSocket, data, size, 0,
                                  reinterpret_cast<const sockaddr*>(&mDestAddr),
                                  sizeof(mDestAddr));
    if (sent < 0 && mRunning.load()) {
        ALOGW("RtpStreamer: UDP sendto failed: %s", strerror(errno));
    }
}

} // namespace rearview
