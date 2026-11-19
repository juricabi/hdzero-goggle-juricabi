#pragma once

#include "H264VideoFileServerMediaSubsession.hh"

class H264LiveVideoServerMediaSubssion : public H264VideoFileServerMediaSubsession {

public:
	// nalTimestamps: stream with audio, see LiveNalVideoSource.hh
	static H264LiveVideoServerMediaSubssion* createNew(UsageEnvironment& env, Boolean reuseFirstSource, Boolean nalTimestamps = False);

protected:
	H264LiveVideoServerMediaSubssion(UsageEnvironment& env, Boolean reuseFirstSource, Boolean nalTimestamps);
	~H264LiveVideoServerMediaSubssion();

protected:
	FramedSource* createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate);

private:
	Boolean fNalTimestamps;
};
