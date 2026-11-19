#include "LiveNalVideoSource.hh"

#include <stdio.h>
#include <string.h>

extern "C"
{
#include "avshare.h"
}

#define VIDEO_POLL_US       1000   // same polling as H264FramedLiveSource
#define VIDEO_IDLE_POLL_US  20000  // ring not attached / reader not connected

LiveNalVideoSource* LiveNalVideoSource::createNew(UsageEnvironment& env, Boolean h265) {
    return new LiveNalVideoSource(env, h265);
}

LiveNalVideoSource::LiveNalVideoSource(UsageEnvironment& env, Boolean h265)
    : FramedSource(env), fH265(h265), fFrame(new unsigned char[MAXSTREAMBUF]), fFrameLen(0), fPos(0),
      fLastNalOffset(0), fFrameHasVcl(False), fEndsAccessUnit(False) {
    fFramePresentationTime.tv_sec = 0;
    fFramePresentationTime.tv_usec = 0;
    printf("[MEDIA SERVER]live NAL video source opened\n");
    avshare_startMedia(MEDIA_VIDEO);
}

LiveNalVideoSource::~LiveNalVideoSource() {
    envir().taskScheduler().unscheduleDelayedTask(nextTask());
    avshare_stopMedia(MEDIA_VIDEO);
    delete[] fFrame;
    printf("[MEDIA SERVER]live NAL video source closed\n");
}

void LiveNalVideoSource::doGetNextFrame() {
    deliverNal();
}

void LiveNalVideoSource::pollAgain(void* clientData) {
    ((LiveNalVideoSource*)clientData)->deliverNal();
}

Boolean LiveNalVideoSource::isVcl(unsigned char nalHeader) const {
    if (fH265) {
        return ((nalHeader >> 1) & 0x3F) < 32; // H.265: types 0..31
    }
    unsigned type = nalHeader & 0x1F;
    return (type >= 1) && (type <= 5); // H.264: coded slices
}

// Next NAL unit of the current frame, without its start code and trailing zero bytes.
Boolean LiveNalVideoSource::nextNal(unsigned& offset, unsigned& length) {
    while (fPos < fFrameLen) {
        unsigned start = fPos;
        unsigned i = start;
        unsigned end = fFrameLen;

        // find the next 00 00 01 start code
        while (i + 2 < fFrameLen) {
            if (fFrame[i + 2] > 1) {
                i += 3;
            } else if (fFrame[i] == 0 && fFrame[i + 1] == 0 && fFrame[i + 2] == 1) {
                end = i;
                break;
            } else {
                ++i;
            }
        }
        fPos = (end < fFrameLen) ? end + 3 : fFrameLen;

        while (end > start && fFrame[end - 1] == 0) {
            --end; // leading zero of a 4-byte start code / trailing_zero_8bits
        }
        if (end > start) {
            offset = start;
            length = end - start;
            return True;
        }
    }

    return False;
}

void LiveNalVideoSource::deliverNal() {
    nextTask() = NULL;
    if (!isCurrentlyAwaitingData()) return;

    unsigned offset = 0, length = 0;
    while (!nextNal(offset, length)) {
        uint64_t ts = 0;
        int len = avshare_readMediaFrame(MEDIA_VIDEO, fFrame, MAXSTREAMBUF, &ts);
        if (len == 0 || len == -1) {
            nextTask() = envir().taskScheduler().scheduleDelayedTask((len == 0) ? VIDEO_POLL_US : VIDEO_IDLE_POLL_US,
                                                                     pollAgain, this);
            return;
        }
        if (len < 0) {
            continue; // frame dropped (too large)
        }

        fFrameLen = (unsigned)len;
        fPos = 0;
        avshare_timestampToTimeval(ts, &fFramePresentationTime);

        // skip the first start code; a frame without any is taken as a single NAL unit
        if (fFrameLen >= 3 && fFrame[0] == 0 && fFrame[1] == 0) {
            unsigned i = 0;
            while (i + 2 < fFrameLen && fFrame[i] == 0 && fFrame[i + 1] == 0 && fFrame[i + 2] != 1) {
                ++i;
            }
            if (i + 2 < fFrameLen && fFrame[i + 2] == 1) {
                fPos = i + 3;
            }
        }

        // find the frame's last NAL unit and whether it holds a picture
        unsigned firstPos = fPos, o = 0, l = 0;
        fLastNalOffset = fFrameLen;
        fFrameHasVcl = False;
        while (nextNal(o, l)) {
            fLastNalOffset = o;
            if (isVcl(fFrame[o])) {
                fFrameHasVcl = True;
            }
        }
        fPos = firstPos;
    }

    fEndsAccessUnit = fFrameHasVcl && (offset == fLastNalOffset);

    if (length > fMaxSize) {
        fNumTruncatedBytes = length - fMaxSize;
        fFrameSize = fMaxSize;
        printf("[MEDIA SERVER]live NAL video source: NAL unit truncated [%u/%u]\n", length, fMaxSize);
    } else {
        fNumTruncatedBytes = 0;
        fFrameSize = length;
    }
    memcpy(fTo, fFrame + offset, fFrameSize);
    fPresentationTime = fFramePresentationTime;
    fDurationInMicroseconds = 0;

    nextTask() = envir().taskScheduler().scheduleDelayedTask(0, (TaskFunc*)FramedSource::afterGetting, this);
}

//----------------------------------------------------------------------------------------------
LiveH264VideoStreamDiscreteFramer*
LiveH264VideoStreamDiscreteFramer::createNew(UsageEnvironment& env, LiveNalVideoSource* source) {
    return new LiveH264VideoStreamDiscreteFramer(env, source);
}

LiveH264VideoStreamDiscreteFramer::LiveH264VideoStreamDiscreteFramer(UsageEnvironment& env, LiveNalVideoSource* source)
    : H264VideoStreamDiscreteFramer(env, source, False, False), fLiveSource(source) {
}

Boolean LiveH264VideoStreamDiscreteFramer::nalUnitEndsAccessUnit(u_int8_t /*nal_unit_type*/) {
    return fLiveSource->endsAccessUnit();
}

LiveH265VideoStreamDiscreteFramer*
LiveH265VideoStreamDiscreteFramer::createNew(UsageEnvironment& env, LiveNalVideoSource* source) {
    return new LiveH265VideoStreamDiscreteFramer(env, source);
}

LiveH265VideoStreamDiscreteFramer::LiveH265VideoStreamDiscreteFramer(UsageEnvironment& env, LiveNalVideoSource* source)
    : H265VideoStreamDiscreteFramer(env, source, False, False), fLiveSource(source) {
}

Boolean LiveH265VideoStreamDiscreteFramer::nalUnitEndsAccessUnit(u_int8_t /*nal_unit_type*/) {
    return fLiveSource->endsAccessUnit();
}
