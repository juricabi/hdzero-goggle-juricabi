#include "H264LiveVideoServerMediaSubssion.hh"
#include "H264FramedLiveSource.hh"
#include "H264VideoStreamFramer.hh"
#include "LiveNalVideoSource.hh"

H264LiveVideoServerMediaSubssion* H264LiveVideoServerMediaSubssion::createNew(UsageEnvironment& env, Boolean reuseFirstSource, Boolean nalTimestamps)
{
    return new H264LiveVideoServerMediaSubssion(env, reuseFirstSource, nalTimestamps);
}

H264LiveVideoServerMediaSubssion::H264LiveVideoServerMediaSubssion(UsageEnvironment& env, Boolean reuseFirstSource, Boolean nalTimestamps)
: H264VideoFileServerMediaSubsession(env, NULL, reuseFirstSource)
, fNalTimestamps(nalTimestamps)
{
    printf("[MEDIA SERVER]H264 server media subssion construct\n");
}

H264LiveVideoServerMediaSubssion::~H264LiveVideoServerMediaSubssion()
{
    printf("[MEDIA SERVER]H264 server media subssion destruct\n");
}

FramedSource* H264LiveVideoServerMediaSubssion::createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate)
{
	estBitrate = 4000; //kbps, estimate

	if (fNalTimestamps)
	{
		// stream with audio: NAL units with the frames' own (shared) timestamps
		LiveNalVideoSource* nalSource = LiveNalVideoSource::createNew(envir(), False);
		if (nalSource == NULL)
		{
			return NULL;
		}
		return LiveH264VideoStreamDiscreteFramer::createNew(envir(), nalSource);
	}

	H264FramedLiveSource* liveSource = H264FramedLiveSource::createNew(envir());
	if (liveSource == NULL)
	{
		return NULL;
	}

	// Create a framer for the Video Elementary Stream:
	return H264VideoStreamFramer::createNew(envir(), liveSource);
}
