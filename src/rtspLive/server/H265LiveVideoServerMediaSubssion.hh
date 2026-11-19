#pragma once

#include "H265VideoFileServerMediaSubsession.hh"

class H265LiveVideoServerMediaSubssion : public H265VideoFileServerMediaSubsession {

public:
	// nalTimestamps: stream with audio, see LiveNalVideoSource.hh
	static H265LiveVideoServerMediaSubssion* createNew(UsageEnvironment& env, Boolean reuseFirstSource, Boolean nalTimestamps = False);

protected:
	H265LiveVideoServerMediaSubssion(UsageEnvironment& env, Boolean reuseFirstSource, Boolean nalTimestamps);
	~H265LiveVideoServerMediaSubssion();

protected:
	FramedSource* createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate);

private:
	Boolean fNalTimestamps;
};
