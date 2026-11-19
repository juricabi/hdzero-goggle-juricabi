/******************************************************************************

  Copyright (C), 2016-2017, Divimath Inc.

 ******************************************************************************
  File Name     : avshare.cpp
  Version       : Initial version
  Author        : Divimath Shenzhen
  Created       : 2017/01/06
  Description   :
  History       :
  1.Date        : 2017/01/06
    Author      : james
    Modification: Created file

******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>

#include "avshare.h"
#include "live_context.h"

#define MEM_KEY 0x568066
#define largeStreamSize		10*1024	//0x100000
#define littleStreamSize	5*1024	//0x80000
#define audioStreamSize		1*1024	//0x40000
#define SHAREBUF_COUNT		56		//8*7

#define RESET_IsConnect(p, num)	    {gAvshare.ptrConnect=p; memset(gAvshare.ptrConnect, 0, num);}
#define CHECK_IsConnect(no)	        (gAvshare.ptrConnect[MEDIA_type2index(no)])
#define SET_IsConnect(no, x)	    {gAvshare.ptrConnect[MEDIA_type2index(no)] = x; }
#define MEDIA_type2index(t)         ((t)-MEDIA_VIDEO)
#define CHECK_IsInitialized(ret)    {if(gAvshare.bufShare == NULL) return (ret);}

#define FIFO_NAME    "rtspStream.fifo"//"/tmp/H264_fifo"
#define MEDIA_NUM     2
#define FIFO_EN       1

#define SAVE_FILE_TEST  0
#define SAVE_FILE_PATH  "/mnt/extsd/test_share_rx.264"

#pragma pack(push)
#pragma pack(1)

typedef struct {
	char wflag;     //ÊÇ·ñ¿ÉÐ´£¬µ±Îª0Ê±¿É¶Á²»¿ÉÐ´£¬¶ÁÈ¡½ø³Ì¶ÁÍêÖ®ºóÖÃ1£»µ±Îª1Ê±¿ÉÐ´²»¿É¶Á£¬Ð´Èë½ø³ÌÐ´ÍêºóÖÃ0¡£
	char frameType; //ÊÓÆµÖ¡ÀàÐÍ£º IÖ¡:1 PÖ¡:0
	char mediaType; //Ö¡ÀàÐÍ£ºvedio:0 audio:1
	char buff_type;
	uint64_t  timestamp;
	int  length;
} ShareStream_t;

#pragma pack(pop)

enum _BUFF_TYPE
{
	BUF_BEGIN,
	BUF_MIDDLE,
	BUF_END,
	BUF_BEG_END,
};

typedef struct
{
	int	id;
	uint8_t* bufShare;
	uint8_t* ptrConnect;

	ShareStream_t* bufStreamHead[MEDIA_NUM];
    char * streamBuf[MEDIA_NUM][SHAREBUF_COUNT];
	int idxNext[MEDIA_NUM];
	int posNext[MEDIA_NUM];
#if(!FIFO_EN)
	int fdPipe;
#endif
	//int readOffset;
	int nFrames;
	int nFrameI;
	int nSPS;

#if(SAVE_FILE_TEST)
	FILE*     fileTest;
    pthread_t threadId;
    bool      bExit;
#endif
} AvshareInfo_t;


//----------------------------------------------------------------------------------------------
// global variables
//----------------------------------------------------------------------------------------------
AvshareInfo_t gAvshare = { -1, NULL, NULL };

//----------------------------------------------------------------------------------------------
// avshare initialize
//----------------------------------------------------------------------------------------------
int avshare_init(void)
{
    int j = 0;
    int i = 0;

	int SHARE_CAPACITY = (largeStreamSize + audioStreamSize + sizeof(ShareStream_t)*MEDIA_NUM)*SHAREBUF_COUNT + MEDIA_NUM;

	if(gAvshare.id != -1)
    {
        //avshare_uninit();
        //avshare_flush();
        return 0;
    }

	memset(&gAvshare, 0, sizeof(gAvshare));
    memset(gAvshare.streamBuf, 0, sizeof(gAvshare.streamBuf));

	gAvshare.id = shmget( MEM_KEY, SHARE_CAPACITY, IPC_CREAT|0644 );
	if( gAvshare.id < 0)
	{
		alogd("failed to create share memory\n");
		return -1;
	}

	gAvshare.bufShare = (uint8_t*)shmat(gAvshare.id, NULL, 0);
	if (gAvshare.bufShare == (void*)-1)
	{
		alogd("get share mem address failed\n");
		gAvshare.bufShare = NULL;
		return -2;
	}

	RESET_IsConnect(gAvshare.bufShare, MEDIA_NUM);
 /*        ____________________________________________________________________________________________________
  format:|connect |        (video + audio) header           |          (video + audio) stream                  |
  bytes: |-2(v,a)-|-(sizeof(ShareStream_t)*2*SHAREBUF_COUNT-|-(largeStreamSize+audioStreamSize)*SHAREBUF_COUNT-|
 */

	for(i=0; i<MEDIA_NUM; i++)
	{
		gAvshare.bufStreamHead[i] = (ShareStream_t*)(gAvshare.bufShare + MEDIA_NUM + i*sizeof(ShareStream_t)*SHAREBUF_COUNT);
		for(j=0; j<SHAREBUF_COUNT; j++)
		{
			gAvshare.bufStreamHead[i][j].wflag = 1;
		}
	}

	for(j=0; j<SHAREBUF_COUNT; j++)  //video
	{
		gAvshare.streamBuf[0][j] = (char*)(gAvshare.bufShare + MEDIA_NUM + sizeof(ShareStream_t)*SHAREBUF_COUNT*MEDIA_NUM + j*largeStreamSize);
	}

	for(j=0; j<SHAREBUF_COUNT; j++)  //audio
	{
		gAvshare.streamBuf[1][j] = gAvshare.streamBuf[0][SHAREBUF_COUNT-1] + largeStreamSize + j*audioStreamSize;
	}

    for(i=0; i<MEDIA_NUM; i++)
    {
	    gAvshare.idxNext[i] = 0;
	    gAvshare.posNext[i] = 0;
    }

	//alogd("bufShare=%x, countOfBuffer=%d\n", gAvshare.bufShare, SHAREBUF_COUNT);
#if(!FIFO_EN)
    gAvshare.fdPipe = open(FIFO_NAME, O_RDONLY|O_NONBLOCK);
#endif

	return 0;
}

int avshare_uninit(void)
{
	CHECK_IsInitialized(0);

	gAvshare.ptrConnect = NULL; // before the detach (avshare_dropConnections() from a signal handler)
	if(shmdt(gAvshare.bufShare)==-1)
	{
		aloge("remove share mem failed\n");
	}

	if(0)//shmctl(gAvshare.id,IPC_RMID,NULL) == -1)//remove id
  	{
    	aloge("remove share mem ID failed\n");
 	}

#if(!FIFO_EN)
 	if(gAvshare.fdPipe >= 0)
    {
        close(gAvshare.fdPipe);
        gAvshare.fdPipe = -1;
    }
#endif

    gAvshare.id = -1;
    gAvshare.bufShare = NULL;

   	alogd("uninit share memory\n");

  	return 0;
}

//----------------------------------------------------------------------------------------------
// avshare server
//----------------------------------------------------------------------------------------------
/*
pstStream:流数据
len:流数据长度
frame_type: 1:I帧; 0:P帧，音频帧不生效，可默认赋值为0
millis: 时间戳
streamType: 0:大码流; 1:小码流; 2:音频流
*/

int avshare_add(uint8_t* pstStream, int len, FrameType_e frame_type, uint32_t millis, MediaType_e streamType)
{
    int buff_size[MEDIA_NUM];
	ShareStream_t* pStreamHead = NULL;
	int data_send = 0;
	int data_len = len;
    int connect_flag = 0;

	buff_size[0] = largeStreamSize;
	buff_size[1] = audioStreamSize;

	CHECK_IsInitialized(-2);

	while( data_len > 0)
	{
        connect_flag = MEDIA_type2index(streamType);
		pStreamHead = (ShareStream_t*)&gAvshare.bufStreamHead[connect_flag][gAvshare.idxNext[connect_flag]];

		if(!CHECK_IsConnect(streamType))
		{
			gAvshare.idxNext[connect_flag]= 0;
			return -3;
		}

		int buffer_len = data_len;
		if( buffer_len > buff_size[connect_flag] )
		{
			buffer_len = buff_size[connect_flag];
		}

		//alogd("debug --share_stream_header[%d][%d]---AddToShareStream-1--%d,%d,%d,%d\n",streamType,indexOfBuf[streamType],pShareStreamHeader->wflag,streamType,pShareStreamHeader->frameType,frame_type);
		//alogd("debug -----AddToShareStream-1--%d,%d,%d,%d\n",pShareStreamHeader->wflag,streamType,pShareStreamHeader->frameType,frame_type);
		if(pStreamHead->wflag == 1)
		{
			pStreamHead->frameType = frame_type;
			pStreamHead->timestamp = millis;
			pStreamHead->mediaType = streamType;

			memcpy((uint8_t*)gAvshare.streamBuf[connect_flag][gAvshare.idxNext[connect_flag]], pstStream+data_send, buffer_len);

			pStreamHead->length    = buffer_len;
			if( data_send == 0 )//begin
			{
				if( data_len > buff_size[connect_flag] )
				{
					pStreamHead->buff_type   = BUF_BEGIN;
                    //alogd("frame begin %d\n", gAvshare.idxNext);
				}
				else
				{
					pStreamHead->buff_type   = BUF_BEG_END;
                    //alogd("frame begin end %d\n", gAvshare.idxNext);
				}
			}
			else
			{
				if( data_len > buff_size[connect_flag] )
				{
					pStreamHead->buff_type   = BUF_MIDDLE;
				}
				else
				{
					pStreamHead->buff_type   = BUF_END;
                    //alogd("frame end %d\n", gAvshare.idxNext);
				}
			}

			pStreamHead->wflag = 0;
			gAvshare.idxNext[connect_flag] = (gAvshare.idxNext[connect_flag]+1) % SHAREBUF_COUNT;

			data_len -= buffer_len;
			data_send += buffer_len;
		}
		else  //对有连接却没读取数据的情况做处理
		{
			return 1;
		}
	}

	return 0;
}

//-----------------------------------------------------------------------------------------------------------
void avshare_addaud( unsigned char * buf, int len, uint32_t millis )
{
#if(0)
	//alogd("debug --------------1\n");
	int ret=avshare_add(buf, len, 1, millis, 2); //  add to share memory, Audio stream
	if(ret==1)
	{
		//buffer fuul, dropped
		static int iDropped = 0;

		iDropped++;
		if(iDropped >= 100)
		{
			alogd("share buffer overflowed, dropped %d audio packets\n", iDropped);
		}
	}
#endif
}

void avshare_addvid(uint8_t* pstStream, int len, FrameType_e frameType, uint32_t millis )
{
	static int iDropped = 0;
	int ret = 0;

#if(SAVE_FILE_TEST)
	if( gAvshare.fileTest == NULL ) {
        gAvshare.fileTest = fopen(SAVE_FILE_PATH, "wb");
	}
	if( gAvshare.fileTest != NULL ) {
        fwrite(pstStream, len, 1, gAvshare.fileTest);
	}
#endif

	if(iDropped > 0)
	{
		if(frameType != FRAME_I)
		{
			iDropped++;
			return;
		}
		else
		{
			alogd("share buffer overflowed, dropped %d video packets\n", iDropped);
			iDropped = 0;
		}
	}

	ret = avshare_add(pstStream, len, frameType, millis, MEDIA_VIDEO);// add to share memory
	if( ret == 1 )
	{
		//buffer full, dropped a frame
		iDropped++;
	}
	else if (0 == ret )
	{
		//added a frame
	}
}

void avshare_flush(void)
{
	int j;
    int i;

	if(gAvshare.bufShare != NULL)
	{
        for(i=0; i<MEDIA_NUM; i++)
        {
		    for(j=0; j<SHAREBUF_COUNT; j++)
		    {
		    	gAvshare.bufStreamHead[i][j].wflag = 1;
		    }
		    gAvshare.idxNext[i] = 0;
            gAvshare.posNext[i] = 0;
        }
	}
}

//----------------------------------------------------------------------------------------------
// avshare client
//----------------------------------------------------------------------------------------------
int avshare_connect(int media_type)
{
	CHECK_IsInitialized(-1);

	if(!CHECK_IsConnect(media_type))
	{
		// Start this media from a clean ring (the writer restarts at slot 0
		// while it sees the stream disconnected), then publish the connection.
		int mediaIndex = MEDIA_type2index(media_type);
		int i;
		for(i=0; i<SHAREBUF_COUNT; i++)
		{
			gAvshare.bufStreamHead[mediaIndex][i].wflag = 1;
		}
		gAvshare.idxNext[mediaIndex] = 0;
		gAvshare.posNext[mediaIndex] = 0;
		__sync_synchronize();
		SET_IsConnect(media_type, 1);
	}

	alogd("connect stream %d successfully: %d\n", media_type, CHECK_IsConnect(media_type));

	return 0;
}

int avshare_disconnect(int media_type)
{
	int i=0;

	CHECK_IsInitialized(0);
	SET_IsConnect(media_type, 0);

	//sleep(1);
	usleep(100);//ä¿è¯writeä¸åç»§ç»­ï¼åå° wflagé½ç½®ä¸º1ï¼writeçindexOfBufå°è¢«ç½®ä¸º0;

	for(i=0; i<SHAREBUF_COUNT; i++)
	{
		gAvshare.bufStreamHead[MEDIA_type2index(media_type)][i].wflag = 1;
	}

	alogd("disconnect stream %d successfully\n", media_type);

	return 0;
}

int avshare_readFrame(uint8_t*sFrameBuf, int nBufLen, struct timeval* timestamp, unsigned* uDurationInMicroseconds)
{
	ShareStream_t* pStreamHead = NULL;
	uint8_t* pStreamData = NULL;
	uint8_t  mediaIndex = MEDIA_type2index(MEDIA_VIDEO);
	FrameType_e frameType = FRAME_NUM;
	int frameLen = 0;
	int copyLen = 0;
	int idxBuf = gAvshare.idxNext[mediaIndex];
	uint32_t offset = gAvshare.posNext[mediaIndex];
    int i = 0;

	CHECK_IsInitialized(0);
    pStreamHead = (ShareStream_t*)&gAvshare.bufStreamHead[mediaIndex][idxBuf];

	if( pStreamHead->wflag==1 )
	{
		for(i=0; i<SHAREBUF_COUNT; i++)
        {
            idxBuf = (idxBuf+1) % SHAREBUF_COUNT;
            pStreamHead = (ShareStream_t*)&gAvshare.bufStreamHead[mediaIndex][idxBuf];
            if(pStreamHead->wflag != 1)
            {
                offset = 0;
                break;
            }
        }

		if( i>=SHAREBUF_COUNT )
        {
            gAvshare.idxNext[mediaIndex] = idxBuf;
            gAvshare.posNext[mediaIndex] = 0;
            return -1;
        }
	}

    __sync_synchronize(); // acquire: the slot was seen filled, now read it
    avshare_timestampToTimeval(pStreamHead->timestamp, timestamp);  // CLOCK_MONOTONIC us -> wall clock
    *uDurationInMicroseconds = 0;

    frameType = (FrameType_e)pStreamHead->frameType;
    pStreamData = (uint8_t*)gAvshare.streamBuf[mediaIndex][idxBuf];

	do
    {
        __sync_synchronize(); // acquire: read the slot only after seeing it filled
        copyLen = pStreamHead->length - offset;
        if( frameLen + copyLen <= nBufLen )//MAXSTREAMBUF )
        {
            memcpy( sFrameBuf + frameLen, pStreamData + offset, copyLen );
            frameLen += copyLen;
        }
        else
        {
            copyLen = nBufLen - frameLen;
            memcpy( sFrameBuf + frameLen, pStreamData + offset, copyLen );
            frameLen += copyLen;
            offset += copyLen;
            break;
        }

        __sync_synchronize(); // release: done reading the slot before freeing it
        pStreamHead->wflag = 1;
        idxBuf = (idxBuf+1) % SHAREBUF_COUNT;
        pStreamHead = (ShareStream_t*)&gAvshare.bufStreamHead[mediaIndex][idxBuf];;
        pStreamData = (uint8_t*)gAvshare.streamBuf[mediaIndex][idxBuf];
        offset = 0;
    } while(pStreamHead->wflag != 1);

    gAvshare.idxNext[mediaIndex] = idxBuf;
    gAvshare.posNext[mediaIndex] = offset;

	return frameLen;
}

int avshare_readAudio(uint8_t*sFrameBuf, int nBufLen, struct timeval* timestamp, unsigned* uDurationInMicroseconds)
{
	ShareStream_t* pStreamHead = NULL;
	uint8_t* pStreamData = NULL;
	uint8_t  mediaIndex = MEDIA_type2index(MEDIA_AUDIO);
	int audioLen = 0;
	int idxBuf = gAvshare.idxNext[mediaIndex];
    int bFrameEnd = 0;
    int i = 0;

	CHECK_IsInitialized(0);
    pStreamHead = (ShareStream_t*)&gAvshare.bufStreamHead[mediaIndex][idxBuf];

	if( pStreamHead->wflag==1 )
	{
		for(i=0; i<SHAREBUF_COUNT; i++)
        {
            idxBuf = (idxBuf+1) % SHAREBUF_COUNT;
            pStreamHead = (ShareStream_t*)&gAvshare.bufStreamHead[mediaIndex][idxBuf];
            if(pStreamHead->wflag != 1)
            {
                break;
            }
        }

		if( i>=SHAREBUF_COUNT )
        {
            //alogd("No frame to be sent...\n");
            if(audioLen <= 0)
            {
                gAvshare.idxNext[mediaIndex] = idxBuf;
            }

            return -1;
        }
	}
	if( (pStreamHead->buff_type != BUF_BEGIN)  && (pStreamHead->buff_type != BUF_BEG_END) )
	{
		alogd("No audio frame start found... %d-%d\n", idxBuf, pStreamHead->buff_type);
        pStreamHead->wflag = 1;
        idxBuf = (idxBuf+1) % SHAREBUF_COUNT;
        if(audioLen <= 0)
        {
            gAvshare.idxNext[mediaIndex] = idxBuf;
        }

		return -2;
	}

    timestamp->tv_sec = (pStreamHead->timestamp/90000);     //s
    timestamp->tv_usec = (pStreamHead->timestamp*100/9);    //us
    *uDurationInMicroseconds = 0;

	while(bFrameEnd == 0)
    {

        pStreamData = (uint8_t*)gAvshare.streamBuf[mediaIndex][idxBuf];
        //sendBuf = streamBuf[stream][indexOfBuf[stream]];
        if( audioLen + pStreamHead->length <= nBufLen )//MAXSTREAMBUF )
        {
            memcpy( sFrameBuf + audioLen, pStreamData, pStreamHead->length );
            audioLen += pStreamHead->length;
        }
        else
        {
            alogd("polldata audio err, out of buf size size=%d", audioLen + pStreamHead->length );
        }

        if( (pStreamHead->buff_type == BUF_END) || (pStreamHead->buff_type == BUF_BEG_END) )
        {
            //a whole frame found
            //alogd("frame buffer %d - %d\n", gAvshare.idxNext, idxBuf);
            bFrameEnd = 1;
        }

        idxBuf = (idxBuf+1) % SHAREBUF_COUNT;
        if(!bFrameEnd)
        {
            pStreamHead = (ShareStream_t*)&gAvshare.bufStreamHead[mediaIndex][idxBuf];;
            if( pStreamHead->wflag==1 )
            {
                //not a complete frame
                bFrameEnd = 2;
            }
            else if(idxBuf == gAvshare.idxNext[mediaIndex])
            {
                //buffer empty
                bFrameEnd = 3;
            }
        }
    }

    if(bFrameEnd == 1)
    {
        //alogd("audioLen - %d, timestamp - %d / %d\n", audioLen, (timestamp->tv_usec-timestampLast.tv_usec), \
        //          (fTimeNow.tv_usec-fTimeLast.tv_usec));

        //release frame buffers
        if(idxBuf < gAvshare.idxNext[mediaIndex])
        {
            for(i=gAvshare.idxNext[mediaIndex]; i<SHAREBUF_COUNT; i++)
            {
                pStreamHead = (ShareStream_t*)&gAvshare.bufStreamHead[mediaIndex][i];
                pStreamHead->wflag = 1;
            }
            for(i=0; i<idxBuf; i++)
            {
                pStreamHead = (ShareStream_t*)&gAvshare.bufStreamHead[mediaIndex][i];
                pStreamHead->wflag = 1;
            }
        }
        else
        {
            for(i=gAvshare.idxNext[mediaIndex]; i<idxBuf; i++)
            {
                pStreamHead = (ShareStream_t*)&gAvshare.bufStreamHead[mediaIndex][i];
                pStreamHead->wflag = 1;
            }
        }

        gAvshare.idxNext[mediaIndex] = idxBuf;
    }
    else
    {
        audioLen = 0;
    }

	return audioLen;
}

#if(SAVE_FILE_TEST)
//----------------------------------------------------------------------------------------------
// avshare thread
//----------------------------------------------------------------------------------------------
void* avshare_task(void * param)
{
    AvshareInfo_t* shareInfo = (AvshareInfo_t*)param;
    int fMaxSize = 2*1024*1024;
    uint8_t* fTo = (uint8_t*)malloc(fMaxSize);
    struct timeval timeStamp;
    unsigned uDurationInMicroseconds;

    gAvshare.fileTest = fopen(SAVE_FILE_PATH, "wb");

    //成功绑定之后，便开始监听
    while(!shareInfo->bExit)
    {
        int nLen = avshare_readFrame(fTo, fMaxSize, &timeStamp, &uDurationInMicroseconds);
        if(nLen < 0)
        {
            usleep(10000);
        }
        else {
            if( gAvshare.fileTest != NULL ) {
                fwrite(fTo, nLen, 1, gAvshare.fileTest);
            }
        }
    }

    free(fTo);
    fclose(gAvshare.fileTest);

    return (void*)0;
}
#endif

bool avshare_start(void)
{
    avshare_startMedia(MEDIA_VIDEO);

#if(SAVE_FILE_TEST)
    gAvshare.bExit = false;
    pthread_create(&gAvshare.threadId, NULL, avshare_task, &gAvshare);
#endif

    return true;
}

void avshare_stop(void)
{
    if( gAvshare.bufShare == NULL ) {
        return;
    }

#if(SAVE_FILE_TEST)
    if( gAvshare.threadId != 0 ) {
        gAvshare.bExit = true;
        pthread_join(gAvshare.threadId, NULL);
        gAvshare.threadId = 0;
    }
#endif
    avshare_stopMedia(MEDIA_VIDEO);
}

//----------------------------------------------------------------------------------------------
// Live stream audio (RTSP AAC track) support
//
// Several sources in this process may use the ring at the same time (a video source and an
// audio source), so the shared memory is attached once and every media is connected/disconnected
// on its own. With only the video source in use this does exactly what avshare_start()/
// avshare_stop() always did: attach + connect on start, disconnect + detach on stop.
//
// Ring protocol (one writer: the record process, one reader per media: this process):
//   wflag == 1: slot free (writer may fill it), wflag == 0: slot filled (reader may read it).
//   A frame occupies one or more consecutive slots: BUF_BEG_END, or BUF_BEGIN [BUF_MIDDLE...] BUF_END.
//   The writer never overwrites a filled slot: when the ring is full it drops the new frame.
//   timestamp: CLOCK_MONOTONIC in microseconds (see src/record/live_audio.c).
//----------------------------------------------------------------------------------------------
#define SLOT_WFLAG(h)   (*(volatile char*)&((h)->wflag))
#define IS_FRAME_START(t) (((t) == BUF_BEGIN) || ((t) == BUF_BEG_END))
#define IS_FRAME_END(t)   (((t) == BUF_END) || ((t) == BUF_BEG_END))

static int gMediaUsers[MEDIA_NUM] = {0, 0};

int avshare_startMedia(int media_type)
{
    int mediaIndex = MEDIA_type2index(media_type);
    if( (mediaIndex < 0) || (mediaIndex >= MEDIA_NUM) ) {
        return -1;
    }

    if( (gMediaUsers[0] + gMediaUsers[1] == 0) || (gAvshare.bufShare == NULL) ) {
        if( avshare_init() != 0 ) {
            return -1;
        }
    }

    if( gMediaUsers[mediaIndex]++ == 0 ) {
        avshare_connect(media_type);
    }

    return 0;
}

void avshare_stopMedia(int media_type)
{
    int mediaIndex = MEDIA_type2index(media_type);
    if( (mediaIndex < 0) || (mediaIndex >= MEDIA_NUM) || (gMediaUsers[mediaIndex] == 0) ) {
        return;
    }

    if( --gMediaUsers[mediaIndex] == 0 ) {
        avshare_disconnect(media_type);
    }

    if( gMediaUsers[0] + gMediaUsers[1] == 0 ) {
        avshare_uninit();
    }
}

// Called on the way out (exit, SIGTERM/SIGINT/SIGHUP, crash): clear this process' connect flags,
// so the record process stops the live encoders at once. Only stores to the shared memory: safe
// in a signal handler. (The record process also notices a reader that died without it.)
void avshare_dropConnections(void)
{
    uint8_t* volatile connect = gAvshare.ptrConnect;
    int i;

    if( (gAvshare.bufShare == NULL) || (connect == NULL) ) {
        return;
    }
    for( i=0; i<MEDIA_NUM; i++ ) {
        if( gMediaUsers[i] > 0 ) {
            ((volatile uint8_t*)connect)[i] = 0;
        }
    }
}

void avshare_timestampToTimeval(uint64_t timestampUs, struct timeval* tv)
{
    // The record process stamps frames with CLOCK_MONOTONIC (shared by both processes).
    // live555 wants wall-clock presentation times (RTCP sender reports pair them with
    // gettimeofday()), so add one process-wide offset: audio and video stay consistent.
    static bool     bOffsetValid = false;
    static int64_t  offsetUs = 0;

    if( !bOffsetValid ) {
        struct timespec ts;
        struct timeval now;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        gettimeofday(&now, NULL);
        offsetUs = ((int64_t)now.tv_sec * 1000000 + now.tv_usec) -
                   ((int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
        bOffsetValid = true;
    }

    int64_t wallUs = (int64_t)timestampUs + offsetUs;
    tv->tv_sec  = (time_t)(wallUs / 1000000);
    tv->tv_usec = (suseconds_t)(wallUs % 1000000);
}

// Find the slot holding the oldest frame start, freeing orphan slots (the tail of a frame whose
// beginning was dropped). Returns the slot index, or -1 if nothing is ready.
static int avshare_findFrameStart(int mediaIndex)
{
    ShareStream_t* heads = gAvshare.bufStreamHead[mediaIndex];
    int idx = gAvshare.idxNext[mediaIndex];
    int n;

    if( SLOT_WFLAG(&heads[idx]) != 0 ) {
        // Nothing where we expect the next frame. Look elsewhere: after a reconnect the writer
        // may still be writing from another position.
        int found = -1;
        for( n=1; n<SHAREBUF_COUNT; n++ ) {
            int j = (idx + n) % SHAREBUF_COUNT;
            if( SLOT_WFLAG(&heads[j]) == 0 ) {
                found = j;
                break;
            }
        }
        if( found < 0 ) {
            return -1;
        }
        __sync_synchronize();
        // The writer fills slots in order, so if it had just filled our slot we would see it now.
        if( SLOT_WFLAG(&heads[idx]) != 0 ) {
            idx = found;
        }
    }
    __sync_synchronize();

    for( n=0; n<SHAREBUF_COUNT; n++ ) {
        ShareStream_t* h = &heads[idx];
        if( SLOT_WFLAG(h) != 0 ) {
            gAvshare.idxNext[mediaIndex] = idx;
            return -1;
        }
        if( IS_FRAME_START(h->buff_type) ) {
            gAvshare.idxNext[mediaIndex] = idx;
            return idx;
        }
        // orphan BUF_MIDDLE/BUF_END: release it
        __sync_synchronize();
        SLOT_WFLAG(h) = 1;
        idx = (idx + 1) % SHAREBUF_COUNT;
    }

    gAvshare.idxNext[mediaIndex] = idx;
    return -1;
}

// A frame the writer gave up half-way (the video writer, avshare_add(), stops when the ring is
// full and then drops frames until the next I frame) is discarded when the next frame start
// appears behind it. But while it is kept, its slots are not free: a next frame larger than the
// rest of the ring (an I frame above half the ring, ~280 KiB) cannot complete either and would be
// kept the same way, frame after frame. The writer fills a frame's slots in one go, so a partial
// frame that has not grown for PARTIAL_TIMEOUT_US is given up here too.
#define PARTIAL_TIMEOUT_US  100000

static struct {
    int      start;    // first slot of the partial frame seen last, -1: none
    int      nSlots;   // filled slots it had
    uint64_t sinceUs;  // when it was first seen that way
} gPartial[MEDIA_NUM] = { {-1, 0, 0}, {-1, 0, 0} };

static uint64_t avshare_nowUs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static bool avshare_partialStalled(int mediaIndex, int start, int nSlots)
{
    uint64_t now = avshare_nowUs();
    if( (gPartial[mediaIndex].start != start) || (gPartial[mediaIndex].nSlots != nSlots) ) {
        gPartial[mediaIndex].start = start;
        gPartial[mediaIndex].nSlots = nSlots;
        gPartial[mediaIndex].sinceUs = now;
        return false;
    }
    return (now - gPartial[mediaIndex].sinceUs) >= PARTIAL_TIMEOUT_US;
}

int avshare_readMediaFrame(int media_type, uint8_t* buf, int bufLen, uint64_t* timestampUs)
{
    int mediaIndex = MEDIA_type2index(media_type);
    int attempt;

    CHECK_IsInitialized(-1);
    if( (mediaIndex < 0) || (mediaIndex >= MEDIA_NUM) || !CHECK_IsConnect(media_type) ) {
        return -1;
    }

    ShareStream_t* heads = gAvshare.bufStreamHead[mediaIndex];

    // A partial frame followed by a new frame start is discarded and the scan restarts once.
    for( attempt=0; attempt<2; attempt++ ) {
        int start = avshare_findFrameStart(mediaIndex);
        if( start < 0 ) {
            return 0;
        }

        int idx = start;
        int len = 0;
        int nSlots = 0;
        bool overflow = false;
        bool restart = false;
        bool complete = false;
        bool stalled = false;
        uint64_t ts = heads[start].timestamp;

        while( nSlots < SHAREBUF_COUNT ) {
            ShareStream_t* h = &heads[idx];
            if( SLOT_WFLAG(h) != 0 ) {
                // writer has not finished this frame yet: try again later (unless it gave up)
                if( (nSlots == 0) || !avshare_partialStalled(mediaIndex, start, nSlots) ) {
                    return 0;
                }
                stalled = true;
                break;
            }
            // acquire: read the slot's header and data only after seeing it filled (ARMv7 may
            // satisfy these loads before the wflag load: a control dependency does not order loads)
            __sync_synchronize();
            if( (nSlots > 0) && IS_FRAME_START(h->buff_type) ) {
                // the writer gave up on the previous frame (ring was full): drop the partial frame
                restart = true;
                break;
            }

            int length = h->length;
            if( (length < 0) || (length > ((mediaIndex == 0) ? (largeStreamSize) : (audioStreamSize))) ) {
                length = 0;
            }
            if( !overflow && (len + length <= bufLen) ) {
                memcpy(buf + len, gAvshare.streamBuf[mediaIndex][idx], length);
                len += length;
            }
            else {
                overflow = true;
            }

            nSlots++;
            complete = IS_FRAME_END(h->buff_type);
            idx = (idx + 1) % SHAREBUF_COUNT;
            if( complete ) {
                break;
            }
        }

        // release the slots of this frame (or of the partial frame being dropped)
        __sync_synchronize();
        int i;
        for( i=0; i<nSlots; i++ ) {
            SLOT_WFLAG(&heads[(start + i) % SHAREBUF_COUNT]) = 1;
        }
        gAvshare.idxNext[mediaIndex] = idx;
        gAvshare.posNext[mediaIndex] = 0;
        gPartial[mediaIndex].start = -1;

        if( stalled ) {
            alogd("drop partial %s frame (%d slots), the writer gave up on it\n", (mediaIndex == 0) ? "video" : "audio", nSlots);
            return -2;
        }
        if( restart ) {
            continue;
        }
        if( !complete ) {
            // a frame filling the whole ring without an end: unusable
            return -2;
        }
        if( overflow ) {
            alogd("drop %s frame, larger than %d bytes\n", (mediaIndex == 0) ? "video" : "audio", bufLen);
            return -2;
        }

        *timestampUs = ts;
        return len;
    }

    return 0;
}

int avshare_peekAudio(uint8_t* buf, int bufLen)
{
    int mediaIndex = MEDIA_type2index(MEDIA_AUDIO);

    CHECK_IsInitialized(-1);
    if( !CHECK_IsConnect(MEDIA_AUDIO) ) {
        return -1;
    }

    int start = avshare_findFrameStart(mediaIndex);
    if( start < 0 ) {
        return 0;
    }

    ShareStream_t* h = &gAvshare.bufStreamHead[mediaIndex][start];
    int len = h->length;
    if( len > bufLen ) {
        len = bufLen;
    }
    if( len < 0 ) {
        len = 0;
    }
    memcpy(buf, gAvshare.streamBuf[mediaIndex][start], len);

    return len;
}
