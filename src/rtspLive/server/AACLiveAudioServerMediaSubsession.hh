#pragma once

// Live stream audio (RTSP AAC track): on-demand subsession announcing
// "MPEG4-GENERIC/<rate>/<channels>" (RFC 3640, mode AAC-hbr) fed by LiveAudioSource.
//
// The format (object type, rate, channels -> "config=") is read from the first ADTS header the
// record process queues once we connect. If no frame comes in time the subsession leaves the
// SDP (live555 skips a subsession without SDP lines): the session is then the video-only one.
// That outcome is remembered for a while, so later DESCRIBEs are not delayed again (see the .cpp).

#include <OnDemandServerMediaSubsession.hh>

#include "LiveAudioSource.hh"

class AACLiveAudioServerMediaSubsession: public OnDemandServerMediaSubsession {
public:
    static AACLiveAudioServerMediaSubsession* createNew(UsageEnvironment& env, Boolean reuseFirstSource);

protected:
    AACLiveAudioServerMediaSubsession(UsageEnvironment& env, Boolean reuseFirstSource);
    virtual ~AACLiveAudioServerMediaSubsession();

protected: // redefined virtual functions
    virtual char const* sdpLines(int addressFamily);
    virtual FramedSource* createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate);
    virtual RTPSink* createNewRTPSink(Groupsock* rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
                                      FramedSource* inputSource);

private:
    Boolean    fHaveAnnounced;
    AdtsHeader fAnnounced; // format of the SDP lines live555 keeps (fSDPLines)
};
