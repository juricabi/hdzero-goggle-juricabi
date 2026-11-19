#include "AACLiveAudioServerMediaSubsession.hh"

#include <MPEG4GenericRTPSink.hh>
#include <stdio.h>
#include <string.h>

extern "C"
{
#include "avshare.h"
}

#define FORMAT_CHECK_MS    10
#define FORMAT_TIMEOUT_MS  2500  // the record process starts its audio chain when we connect
#define FORMAT_RETRY_MS    10000 // after a timeout, DESCRIBEs leave the track out without waiting

//----------------------------------------------------------------------------------------------
// Waiting for the first audio frame (its ADTS header gives the format for the SDP).
//
// live555 asks a subsession for its SDP lines twice per DESCRIBE and does not keep a NULL
// answer, and DESCRIBEs of different clients nest (a wait runs a nested event loop, during which
// the next DESCRIBE is handled). So there is one wait for the whole process: one deadline shared
// by every waiter, a "done" flag per waiter (so nested loops all return when it ends), and a
// timeout is remembered for FORMAT_RETRY_MS, during which DESCRIBEs leave the track out at once.
// A DESCRIBE is therefore delayed at most FORMAT_TIMEOUT_MS, and at most once per retry period.
//----------------------------------------------------------------------------------------------
namespace {

struct FormatWaiter {
    char done;
    FormatWaiter* next;
};

struct FormatWait {
    UsageEnvironment* env;
    TaskToken task;
    FormatWaiter* waiters;     // non-NULL while a wait runs
    uint64_t deadlineUs;
    Boolean timedOut;          // the last wait timed out ...
    uint64_t noRetryBeforeUs;  // ... and no new wait starts before this time
};

FormatWait gWait = {NULL, NULL, NULL, 0, False, 0};

void formatWaitDone(Boolean found) {
    FormatWaiter* w = gWait.waiters;
    gWait.waiters = NULL;
    for (; w != NULL; w = w->next) {
        w->done = ~0;
    }

    if (!found) {
        printf("[MEDIA SERVER]live audio: no audio frame within %u ms, audio track not announced (no new wait for %u s)\n",
               FORMAT_TIMEOUT_MS, FORMAT_RETRY_MS / 1000);
    }
}

void formatWaitCheck(void* /*clientData*/) {
    gWait.task = NULL;

    // a source of this process may have seen a frame, else look at the ring
    AdtsHeader hdr;
    if (!LiveAudioTrack::format(hdr)) {
        unsigned char buf[16];
        int len = avshare_peekAudio(buf, sizeof(buf));
        if ((len > 0) && aacParseAdtsHeader(buf, (unsigned)len, hdr)) {
            LiveAudioTrack::noteFrame(hdr);
        }
    }
    if (LiveAudioTrack::format(hdr)) {
        gWait.timedOut = False;
        formatWaitDone(True);
        return;
    }

    uint64_t now = liveMonotonicUs();
    if (now >= gWait.deadlineUs) {
        gWait.timedOut = True;
        gWait.noRetryBeforeUs = now + (uint64_t)FORMAT_RETRY_MS * 1000;
        formatWaitDone(False);
        return;
    }

    gWait.task = gWait.env->taskScheduler().scheduleDelayedTask(FORMAT_CHECK_MS * 1000, formatWaitCheck, NULL);
}

// a wait timed out recently: do not wait again yet
Boolean formatWaitBlocked() {
    return (gWait.waiters == NULL) && gWait.timedOut && (liveMonotonicUs() < gWait.noRetryBeforeUs);
}

// The track format, waiting (bounded, see above) for the first frame if needed.
Boolean formatWait(UsageEnvironment& env, AdtsHeader& hdr) {
    if (LiveAudioTrack::format(hdr)) {
        return True;
    }
    if (formatWaitBlocked()) {
        return False;
    }

    FormatWaiter self = {0, gWait.waiters};
    if (gWait.waiters == NULL) {
        // start a wait; a nested DESCRIBE joins the running one and ends with it
        gWait.env = &env;
        gWait.deadlineUs = liveMonotonicUs() + (uint64_t)FORMAT_TIMEOUT_MS * 1000;
    }
    gWait.waiters = &self;
    if (gWait.task == NULL) {
        formatWaitCheck(NULL); // may end the wait right away
    }
    if (!self.done) {
        env.taskScheduler().doEventLoop(&self.done);
    }

    return LiveAudioTrack::format(hdr);
}

Boolean sameFormat(AdtsHeader const& a, AdtsHeader const& b) {
    return (a.profile == b.profile) && (a.sfIndex == b.sfIndex) && (a.channelConfig == b.channelConfig);
}

} // namespace

//----------------------------------------------------------------------------------------------
AACLiveAudioServerMediaSubsession*
AACLiveAudioServerMediaSubsession::createNew(UsageEnvironment& env, Boolean reuseFirstSource) {
    return new AACLiveAudioServerMediaSubsession(env, reuseFirstSource);
}

AACLiveAudioServerMediaSubsession::AACLiveAudioServerMediaSubsession(UsageEnvironment& env, Boolean reuseFirstSource)
    : OnDemandServerMediaSubsession(env, reuseFirstSource), fHaveAnnounced(False) {
    memset(&fAnnounced, 0, sizeof(fAnnounced));
    printf("[MEDIA SERVER]AAC server media subsession construct\n");
}

AACLiveAudioServerMediaSubsession::~AACLiveAudioServerMediaSubsession() {
    printf("[MEDIA SERVER]AAC server media subsession destruct\n");
}

char const* AACLiveAudioServerMediaSubsession::sdpLines(int addressFamily) {
    AdtsHeader current;
    Boolean known = LiveAudioTrack::format(current);

    if ((fSDPLines != NULL) && !(known && fHaveAnnounced && sameFormat(current, fAnnounced))) {
        // the audio stopped (or changed format) since these lines were made: make them again
        delete[] fSDPLines;
        fSDPLines = NULL;
    }
    if ((fSDPLines == NULL) && !known && formatWaitBlocked()) {
        // a wait timed out recently: leave the track out, without opening a source again
        return NULL;
    }

    return OnDemandServerMediaSubsession::sdpLines(addressFamily);
}

FramedSource* AACLiveAudioServerMediaSubsession::createNewStreamSource(unsigned /*clientSessionId*/, unsigned& estBitrate) {
    estBitrate = 96; // kbps, estimate (AAC 64 kbps + RTP/UDP/IP overhead)
    return LiveAudioSource::createNew(envir());
}

RTPSink* AACLiveAudioServerMediaSubsession::createNewRTPSink(Groupsock* rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
                                                             FramedSource* inputSource) {
    // Read the format the encoder really produces from the first ADTS header (the source that
    // was just created makes the record process start encoding), like H264VideoFileServer-
    // MediaSubsession waits for SPS/PPS.
    AdtsHeader fmt;
    if (!formatWait(envir(), fmt)) {
        return NULL; // no SDP lines: the session is announced without audio
    }
    fAnnounced = fmt;
    fHaveAnnounced = True;

    ((LiveAudioSource*)inputSource)->setExpectedFormat(fmt.profile, fmt.sfIndex, fmt.channelConfig);

    char config[8];
    aacConfigString(fmt.profile + 1, fmt.sfIndex, fmt.channelConfig, config);
    unsigned numChannels = (fmt.channelConfig == 7) ? 8 : fmt.channelConfig;
    return MPEG4GenericRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
                                          aacSamplingFrequency(fmt.sfIndex), "audio", "AAC-hbr",
                                          config, numChannels);
}
