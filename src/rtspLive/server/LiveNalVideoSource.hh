#pragma once

// Live stream video for the stream with audio (RTSP AAC track enabled).
//
// The default video path (H264FramedLiveSource + H264/H265VideoStreamFramer) parses the byte
// stream and makes up presentation times from the frame rate, which drift away from the real
// capture times audio is stamped with. This source instead delivers the NAL units of each frame
// in the ring with the frame's own timestamp, through a discrete framer: video and audio share
// one clock, and no frame waits for the next one's start code.

#include <FramedSource.hh>
#include <H264VideoStreamDiscreteFramer.hh>
#include <H265VideoStreamDiscreteFramer.hh>

class LiveNalVideoSource: public FramedSource {
public:
    static LiveNalVideoSource* createNew(UsageEnvironment& env, Boolean h265);

    // The NAL unit delivered last ends its access unit: it is the last NAL unit of a ring frame
    // that holds a picture (a VCL NAL unit). Ring frames are the encoder's output frames, all
    // NAL units of one carry its timestamp (RFC 6184 / 7798: the marker goes on the last packet
    // of the access unit, whatever its type, e.g. a trailing SEI). A frame without a picture
    // (the parameter sets queued before an I frame) does not end the access unit.
    Boolean endsAccessUnit() const { return fEndsAccessUnit; }

protected:
    LiveNalVideoSource(UsageEnvironment& env, Boolean h265);
    virtual ~LiveNalVideoSource();

private:
    virtual void doGetNextFrame();
    static void pollAgain(void* clientData);
    void deliverNal();
    Boolean nextNal(unsigned& offset, unsigned& length);
    Boolean isVcl(unsigned char nalHeader) const;

private:
    Boolean fH265;
    unsigned char* fFrame;
    unsigned fFrameLen;
    unsigned fPos;            // parse position in fFrame
    unsigned fLastNalOffset;  // where the last NAL unit of the frame starts
    Boolean fFrameHasVcl;
    struct timeval fFramePresentationTime;
    Boolean fEndsAccessUnit;
};

// Discrete framers that set the RTP marker at the end of each access unit (see above) only
// (the default guesses "any VCL NAL unit", wrong for multi-slice pictures and trailing SEI).
class LiveH264VideoStreamDiscreteFramer: public H264VideoStreamDiscreteFramer {
public:
    static LiveH264VideoStreamDiscreteFramer* createNew(UsageEnvironment& env, LiveNalVideoSource* source);

protected:
    LiveH264VideoStreamDiscreteFramer(UsageEnvironment& env, LiveNalVideoSource* source);
    virtual Boolean nalUnitEndsAccessUnit(u_int8_t nal_unit_type);

private:
    LiveNalVideoSource* fLiveSource;
};

class LiveH265VideoStreamDiscreteFramer: public H265VideoStreamDiscreteFramer {
public:
    static LiveH265VideoStreamDiscreteFramer* createNew(UsageEnvironment& env, LiveNalVideoSource* source);

protected:
    LiveH265VideoStreamDiscreteFramer(UsageEnvironment& env, LiveNalVideoSource* source);
    virtual Boolean nalUnitEndsAccessUnit(u_int8_t nal_unit_type);

private:
    LiveNalVideoSource* fLiveSource;
};
