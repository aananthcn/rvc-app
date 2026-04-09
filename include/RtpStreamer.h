#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <netinet/in.h>

namespace rearview {

// Packetises H.264 Annex-B byte stream as RTP/UDP (RFC 3984).
// Single-NAL packets for small NALs; FU-A fragmentation for large ones.
class RtpStreamer {
public:
    RtpStreamer();
    ~RtpStreamer();

    // Open UDP socket and resolve destination address. Must be called before sendAnnexB().
    bool start();

    // Close the UDP socket.
    void stop();

    // Send one encoder output buffer (Annex-B framed) as one or more RTP packets.
    void sendAnnexB(const uint8_t* data, size_t size, int64_t presentationTimeUs);

private:
    void    sendNalUnit(const uint8_t* nal, size_t size, int64_t rtpTimestamp);
    void    sendSingleNal(const uint8_t* nal, size_t size, int64_t rtpTimestamp, bool marker);
    void    sendFuA(const uint8_t* nal, size_t size, int64_t rtpTimestamp);
    void    writeRtpHeader(uint8_t* buf, bool marker, int64_t rtpTimestamp);
    int64_t toRtpTimestamp(int64_t presentationTimeUs) const;
    void    sendUdp(const uint8_t* data, size_t size);

    int               mSocket{-1};
    sockaddr_in       mDestAddr{};
    std::atomic<bool> mRunning{false};

    uint32_t mSsrc;
    uint16_t mSeqNum;
    uint32_t mTimestampOffset;
};

} // namespace rearview
