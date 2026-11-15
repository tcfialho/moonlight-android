package com.limelight.nvstream.av.video;

public abstract class VideoDecoderRenderer {
    public abstract int setup(int format, int width, int height, int redrawRate);

    public abstract void start();

    public abstract void stop();

    // This is called once for each frame-start NALU. This means it will be called several times
    // for an IDR frame which contains several parameter sets and the I-frame data.
    public abstract int submitDecodeUnit(byte[] decodeUnitData, int decodeUnitLength, int decodeUnitType,
                                         int frameNumber, int frameType, char frameHostProcessingLatency,
                                         long receiveTimeMs, long enqueueTimeMs);
    
    public boolean supportsNativeInput() { return false; }
    public int submitNativeDecodeUnit(long handle, int length, int frameNumber, int frameType,
                                      char hostLatency, long receiveMs, long enqueueMs) {
        throw new UnsupportedOperationException("Native input not supported");
    }
    public abstract void cleanup();

    public abstract int getCapabilities();

    public abstract void setHdrMode(boolean enabled, byte[] hdrMetadata);
}
