#pragma once

// Live stream audio (RTSP AAC track): reads the ADTS frames queued by the record process in the
// shared ring and delivers raw AAC access units (ADTS header removed) for MPEG4GenericRTPSink.

#include <FramedSource.hh>
#include <stdint.h>

struct AdtsHeader {
    unsigned profile;       // audio object type - 1 (1 = AAC-LC)
    unsigned sfIndex;       // sampling frequency index
    unsigned channelConfig;
    unsigned headerLength;  // 7, or 9 with CRC
    unsigned frameLength;   // header + payload
    unsigned numRawBlocks;  // raw data blocks in this ADTS frame
};

// Returns True and fills "hdr" when "data" starts with a valid ADTS header.
Boolean aacParseAdtsHeader(unsigned char const* data, unsigned len, AdtsHeader& hdr);
// Sampling frequency of an ADTS/AudioSpecificConfig frequency index (0 if invalid).
unsigned aacSamplingFrequency(unsigned sfIndex);
// 2-byte AudioSpecificConfig as the 4 hex digits of the SDP "config=" parameter.
void aacConfigString(unsigned audioObjectType, unsigned sfIndex, unsigned channelConfig, char* out /* >= 5 bytes */);

// CLOCK_MONOTONIC in microseconds (the clock of the ring timestamps)
uint64_t liveMonotonicUs();

// What the record process produces on the audio track, as seen by this process. Every audio
// source, and the format check at DESCRIBE, read the same ring: the state is process-wide.
namespace LiveAudioTrack {
    // an audio frame was seen in the ring: its format is the track's format
    void noteFrame(AdtsHeader const& hdr);
    // the track's format, if a frame was seen (and the track has not been given up since)
    Boolean format(AdtsHeader& hdr);
    // a reader waited too long for a frame: forget the format, so the next DESCRIBE checks
    // again whether audio comes (and leaves the track out if not)
    void noteStall(unsigned seconds);
}

class LiveAudioSource: public FramedSource {
public:
    static LiveAudioSource* createNew(UsageEnvironment& env);

    // The stream format announced in the SDP: frames in another format are dropped,
    // because the format cannot change once a client has set up the stream.
    void setExpectedFormat(unsigned profile, unsigned sfIndex, unsigned channelConfig);

protected:
    LiveAudioSource(UsageEnvironment& env);
    virtual ~LiveAudioSource();

private:
    virtual void doGetNextFrame();
    static void pollAgain(void* clientData);
    void deliverFrame();
    unsigned nextPollUs() const;

private:
    unsigned char* fBuffer;
    Boolean  fHaveExpectedFormat;
    AdtsHeader fExpected;
    unsigned fNbFrames;
    unsigned fNbDropped;
    uint64_t fWaitStartUs;  // when the sink asked for the frame being waited for
    uint64_t fLastSeenUs;   // last frame read from the ring
    Boolean  fStallNoted;
    unsigned fFrameUs;      // duration of the last frame
};
