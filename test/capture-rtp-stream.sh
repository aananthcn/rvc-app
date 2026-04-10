#! /bin/bash
# Receives the RTP/H.264 stream from rvc_app and displays it on screen.
# Press Ctrl+C to stop.

gst-launch-1.0 \
    udpsrc port=5004 \
    caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96" \
    ! rtph264depay \
    ! "video/x-h264,stream-format=byte-stream,alignment=au" \
    ! h264parse \
    ! avdec_h264 \
    ! videoconvert \
    ! autovideosink sync=false
