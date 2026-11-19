/******************************************************************************

  Copyright (C), 2016-2017, Divimath Inc.

 ******************************************************************************
  File Name     : avshare.c
  Version       : Initial version
  Author        : Divimath Shenzhen
  Created       : 2017/01/06
  Description   :
  History       :
  1.Date        : 2017/01/06
    Author      : james
    Modification: Created file

******************************************************************************/

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

#include "avshare.h"

#define MEM_KEY          0x568066
#define largeStreamSize  10 * 1024 // 0x100000
#define littleStreamSize 5 * 1024  // 0x80000
#define audioStreamSize  1 * 1024  // 0x40000
#define SHAREBUF_COUNT   56        // 8*7
#define FRAME_LenMAX     (3 * 1024 * 1024)
#define AUDIO_LenMAX     (32 * 1024)

#define RESET_IsConnect(p, num)              \
    {                                        \
        gAvshare.ptrConnect = p;             \
        memset(gAvshare.ptrConnect, 0, num); \
    }
#define CHECK_IsConnect(no) (gAvshare.ptrConnect[MEDIA_type2index(no)])
#define SET_IsConnect(no, x) \
    { gAvshare.ptrConnect[MEDIA_type2index(no)] = x; }
#define MEDIA_type2index(t) ((t)-MEDIA_VIDEO)

#define FIFO_NAME "rtspStream.fifo" //"/tmp/H264_fifo"

#define MEDIA_NUM 2
#define FIFO_EN   1

#define PRINT_INFO 1
#define PRINT_LINE 0

#if (PRINT_INFO)

#if (PRINT_LINE)
#define dpf(fmt, args...) printf("[airsink>%s:%d]" fmt, __FILE__, __LINE__, ##args)
#else
#define dpf printf
#endif

#else
#define dpf(x...)
#endif

#if (PRINT_LINE)
#define dperr(fmt, args...) fprintf(stderr, "[airsink>%s:%d]" fmt, __FILE__, __LINE__, ##args)
#else
#define dperr printf
#endif

#define SAVE_FILE_TEST 0
#define SAVE_FILE_PATH "/mnt/extsd/test_share_tx.264"

#pragma pack(push)
#pragma pack(1)

typedef struct {
    char wflag;     //\CAǷ\F1\BF\C9д\A3\AC\B5\B1Ϊ0ʱ\BFɶ\C1\B2\BB\BF\C9д\A3\AC\B6\C1ȡ\BD\F8\B3̶\C1\CD\EA֮\BA\F3\D6\C31\A3\BB\B5\B1Ϊ1ʱ\BF\C9д\B2\BB\BFɶ\C1\A3\ACд\C8\EB\BD\F8\B3\CCд\CD\EA\BA\F3\D6\C30\A1\A3
    char frameType; //\CA\D3Ƶ֡\C0\E0\D0ͣ\BA I֡:1 P֡:0
    char mediaType; // ֡\C0\E0\D0ͣ\BAvedio:0 audio:1
    char buff_type;
    uint64_t timestamp;
    int length;
} ShareStream_t;

#pragma pack(pop)

enum _BUFF_TYPE {
    BUF_BEGIN,
    BUF_MIDDLE,
    BUF_END,
    BUF_BEG_END,
};

typedef struct
{
    int id;
    uint8_t *bufShare;
    uint8_t *ptrConnect;

    ShareStream_t *bufStreamHead[MEDIA_NUM];
    uint8_t *streamBuf[MEDIA_NUM][SHAREBUF_COUNT];
    int idxNext[MEDIA_NUM];
#if (!FIFO_EN)
    int fdPipe;
#endif
    uint8_t *frameBuf;
    int frameLen;
    int pesLen;
    uint64_t timestamp;

    uint8_t *audioBuf;
    int audioLen;
    int audiopesLen;
    uint64_t audiotimestamp;

#if (SAVE_FILE_TEST)
    FILE *fileTest;
#endif
} AvshareInfo_t;

AvshareInfo_t gAvshare;

int avshare_init(void) {
    int j = 0;
    int i = 0;
    // system_exec("killall rtspServer");//kill rtspServer，通过app_deamon使rtspServer重启，保证rtspServer读取的顺序是正确的
    // sleep(1);

    int SHARE_CAPACITY = (largeStreamSize + audioStreamSize + sizeof(ShareStream_t) * MEDIA_NUM) * SHAREBUF_COUNT + MEDIA_NUM;

    memset(&gAvshare, 0, sizeof(gAvshare));
    memset(gAvshare.streamBuf, 0, sizeof(gAvshare.streamBuf));

    if ((gAvshare.id = shmget(MEM_KEY, SHARE_CAPACITY, IPC_CREAT | 0644)) == -1) {
        dpf("failed to create share memory\n");
        return -1;
    }

    if ((gAvshare.bufShare = shmat(gAvshare.id, NULL, 0)) == (void *)-1) {
        dpf("get share mem address failed\n");
        gAvshare.bufShare = NULL;
        return -2;
    }

    RESET_IsConnect(gAvshare.bufShare, MEDIA_NUM);
    // sleep(1);
    /*        ____________________________________________________________________________________________________
     format:|connect |        (video + audio) header           |          (video + audio) stream                  |
     bytes: |-2(v,a)-|-(sizeof(ShareStream_t)*2*SHAREBUF_COUNT-|-(largeStreamSize+audioStreamSize)*SHAREBUF_COUNT-|
    */

    for (i = 0; i < MEDIA_NUM; i++) {
        gAvshare.bufStreamHead[i] = (ShareStream_t *)(gAvshare.bufShare + MEDIA_NUM + i * sizeof(ShareStream_t) * SHAREBUF_COUNT);
        for (j = 0; j < SHAREBUF_COUNT; j++) {
            gAvshare.bufStreamHead[i][j].wflag = 1;
        }
    }

    for (j = 0; j < SHAREBUF_COUNT; j++) // video
    {
        gAvshare.streamBuf[0][j] = gAvshare.bufShare + MEDIA_NUM + sizeof(ShareStream_t) * SHAREBUF_COUNT * MEDIA_NUM + j * largeStreamSize;
    }

    for (j = 0; j < SHAREBUF_COUNT; j++) // audio
    {
        gAvshare.streamBuf[1][j] = gAvshare.streamBuf[0][SHAREBUF_COUNT - 1] + largeStreamSize + j * audioStreamSize;
    }

    for (i = 0; i < MEDIA_NUM; i++)
        gAvshare.idxNext[i] = 0;

        // dpf("bufShare=%x, countOfBuffer=%d\n", gAvshare.bufShare, SHAREBUF_COUNT);
#if (!FIFO_EN)
    if (access(FIFO_NAME, F_OK) == -1) {
        mkfifo(FIFO_NAME, 0777);
    }
    gAvshare.fdPipe = open(FIFO_NAME, O_WRONLY | O_NONBLOCK);
#endif
    gAvshare.frameBuf = (uint8_t *)malloc(FRAME_LenMAX);
    if (gAvshare.frameBuf == NULL) {
        avshare_uninit();
        return -1;
    }
    gAvshare.audioBuf = (uint8_t *)malloc(AUDIO_LenMAX);
    if (gAvshare.audioBuf == NULL) {
        avshare_uninit();
        return -1;
    }

    return 0;
}

/*
pstStream:流数据
len:流数据长度
frame_type: 1:I帧; 0:P帧，音频帧不生效，可默认赋值为0
millis: 时间戳
streamType: 0:大码流; 1:小码流; 2:音频流
*/

int avshare_add(uint8_t *pstStream, int len, FrameType_e frame_type, uint64_t timestamp, MediaType_e streamType) {
    int buff_size[MEDIA_NUM];
    ShareStream_t *pStreamHead = NULL;
    int data_send = 0;
    int data_len = len;
    int connect_flag = 0;

    buff_size[0] = largeStreamSize;
    buff_size[1] = audioStreamSize;

    if (gAvshare.bufShare == NULL)
        return -2;

    while (data_len > 0) {
        connect_flag = MEDIA_type2index(streamType);
        pStreamHead = (ShareStream_t *)&gAvshare.bufStreamHead[connect_flag][gAvshare.idxNext[connect_flag]];

        if (!CHECK_IsConnect(streamType)) {
            gAvshare.idxNext[connect_flag] = 0;
            return -3;
        }

        int buffer_len = data_len;
        if (buffer_len > buff_size[connect_flag]) {
            buffer_len = buff_size[connect_flag];
        }

        // printf("debug --share_stream_header[%d][%d]---AddToShareStream-1--%d,%d,%d,%d\n",streamType,indexOfBuf[streamType],pShareStreamHeader->wflag,streamType,pShareStreamHeader->frameType,frame_type);
        // printf("debug -----AddToShareStream-1--%d,%d,%d,%d\n",pShareStreamHeader->wflag,streamType,pShareStreamHeader->frameType,frame_type);
        if (pStreamHead->wflag == 1) {
            __sync_synchronize(); // acquire: the reader is done with the slot it freed
            pStreamHead->frameType = frame_type;
            pStreamHead->timestamp = timestamp;
            pStreamHead->mediaType = streamType;

            memcpy((uint8_t *)gAvshare.streamBuf[connect_flag][gAvshare.idxNext[connect_flag]], pstStream + data_send, buffer_len);

            pStreamHead->length = buffer_len;
            if (data_send == 0) // begin
            {
                if (data_len > buff_size[connect_flag]) {
                    pStreamHead->buff_type = BUF_BEGIN;
                    // printf("frame begin %d\n", gAvshare.idxNext);
                } else {
                    pStreamHead->buff_type = BUF_BEG_END;
                    // printf("frame begin end %d\n", gAvshare.idxNext);
                }
            } else {
                if (data_len > buff_size[connect_flag]) {
                    pStreamHead->buff_type = BUF_MIDDLE;
                } else {
                    pStreamHead->buff_type = BUF_END;
                    // printf("frame end %d\n", gAvshare.idxNext);
                }
            }

            __sync_synchronize(); // slot contents must be visible before the reader sees it filled
            pStreamHead->wflag = 0;
            gAvshare.idxNext[connect_flag] = (gAvshare.idxNext[connect_flag] + 1) % SHAREBUF_COUNT;

            data_len -= buffer_len;
            data_send += buffer_len;
        } else // 对有连接却没读取数据的情况做处理
        {
            return 1;
        }
    }

    return 0;
}

int avshare_uninit(void) {
    if (shmdt(gAvshare.bufShare) == -1) {
        dperr("remove share mem failed\n");
    }

    if (shmctl(gAvshare.id, IPC_RMID, NULL) == -1) // remove id
    {
        dperr("remove share mem ID failed");
    }
#if (!FIFO_EN)
    if (gAvshare.fdPipe >= 0) {
        close(gAvshare.fdPipe);
        gAvshare.fdPipe = -1;
    }
#endif
    if (gAvshare.frameBuf != NULL) {
        free(gAvshare.frameBuf);
        gAvshare.frameBuf = NULL;
    }

    if (gAvshare.audioBuf != NULL) {
        free(gAvshare.audioBuf);
        gAvshare.audioBuf = NULL;
    }

    return 0;
}

//-----------------------------------------------------------------------------------------------------------
void avshare_addaud(unsigned char *buf, int len, uint32_t millis) {
#if (0)
    // printf("debug --------------1\n");
    int ret = avshare_add(buf, len, 1, millis, 2); //  add to share memory, Audio stream
    if (ret == 1) {
        // buffer fuul, dropped
        static int iDropped = 0;

        iDropped++;
        if (iDropped >= 100) {
            dpf("share buffer overflowed, dropped %d audio packets\n", iDropped);
        }
    }
#endif
}

void avshare_addvid(uint8_t *pstStream, int len, FrameType_e frameType, uint64_t timestamp) {
    static int iDropped = 0;
    int ret = 0;

#if (SAVE_FILE_TEST)
    if (gAvshare.fileTest == NULL) {
        gAvshare.fileTest = fopen(SAVE_FILE_PATH, "wb");
    }
    if (gAvshare.fileTest != NULL) {
        fwrite(pstStream, len, 1, gAvshare.fileTest);
    }
#endif

    if (iDropped > 0) {
        if (frameType != FRAME_I) {
            iDropped++;
            return;
        } else {
            dpf("share buffer overflowed, dropped %d video packets\n", iDropped);
            iDropped = 0;
        }
    }

    ret = avshare_add(pstStream, len, frameType, timestamp, MEDIA_VIDEO); // add to share memory
    if (ret == 1) {
        // buffer full, dropped a frame
        iDropped++;
    } else if (0 == ret) {
        // added a frame
    }
}

void avshare_flush(void) {
    int j;
    int i;

    if (gAvshare.bufShare != NULL) {
        for (i = 0; i < MEDIA_NUM; i++) {
            for (j = 0; j < SHAREBUF_COUNT; j++) {
                gAvshare.bufStreamHead[i][j].wflag = 1;
            }
            gAvshare.idxNext[i] = 0;
        }
    }
}

int avshare_addAudio(uint8_t *data, int len, uint64_t timestamp, int isNewFrame, int pesLen) {
    if (isNewFrame &&
        (gAvshare.audioLen > 0) && //(gAvshare.timestamp != timestamp) )
        ((gAvshare.audiotimestamp != timestamp) || ((gAvshare.audiopesLen < 65536) && (gAvshare.audioLen == gAvshare.audiopesLen)))) {
        int ret = avshare_add(gAvshare.audioBuf, gAvshare.audioLen, 0, gAvshare.audiotimestamp, 2);
        if (ret == 0) {
            // dpf("add audio %d/%d - %x\n", gAvshare.audiopesLen, gAvshare.audioLen, gAvshare.audioBuf[0]);
        } else if (ret != -3) {
            dpf("add audio %d failed %d\n", gAvshare.audioLen, ret);
        }
        gAvshare.audioLen = 0;
    }

    memcpy(gAvshare.audioBuf + gAvshare.audioLen, data, len);
    gAvshare.audioLen += len;
    gAvshare.audiopesLen = pesLen;
    gAvshare.audiotimestamp = timestamp;

    return 0;
}

int avshare_addPacket(uint8_t *data, int len, uint64_t timestamp, int isNewFrame, int pesLen) {
    if (isNewFrame &&
        (gAvshare.frameLen > 0) && //(gAvshare.timestamp != timestamp) )   65498
        ((gAvshare.timestamp != timestamp) || ((gAvshare.pesLen < 65536) && (gAvshare.frameLen == gAvshare.pesLen)))) {
        int ret = avshare_add(gAvshare.frameBuf, gAvshare.frameLen, FRAME_I, gAvshare.timestamp, MEDIA_VIDEO);
        if (ret == 0) {
            // dpf("add frame %d/%d - %x %x %x %x %x %x\n", gAvshare.pesLen, gAvshare.frameLen, gAvshare.frameBuf[0], gAvshare.frameBuf[1],
            //     gAvshare.frameBuf[2], gAvshare.frameBuf[3], gAvshare.frameBuf[4], gAvshare.frameBuf[5]);
        } else if (ret != -3) {
            dpf("add frame %d failed %d\n", gAvshare.frameLen, ret);
        }
        gAvshare.frameLen = 0;
    }

    memcpy(gAvshare.frameBuf + gAvshare.frameLen, data, len);
    gAvshare.frameLen += len;
    gAvshare.pesLen = pesLen;
    gAvshare.timestamp = timestamp;

    return 0;
}

#if (!FIFO_EN)
int avshare_write(uint8_t *pstStream, int len) {
    if (gAvshare.fdPipe < 0) {
        gAvshare.fdPipe = open(FIFO_NAME, O_WRONLY | O_NONBLOCK);
        if (gAvshare.fdPipe < 0) {
            return 0;
        }
    }

    len = write(gAvshare.fdPipe, pstStream, len);
    if (len < 0) {
        close(gAvshare.fdPipe);
        gAvshare.fdPipe = -1;
    }

    return len;
}
#endif

//----------------------------------------------------------------------------------------------
// avshare client
//----------------------------------------------------------------------------------------------
int avshare_connect(MediaType_e media_type) {
    if (!CHECK_IsConnect(media_type)) {
        SET_IsConnect(media_type, 1);
        gAvshare.idxNext[media_type] = 0;
    }

    dpf("connect stream %d successfully - %d\n", media_type, CHECK_IsConnect(media_type));

    return 0;
}

int avshare_disconnect(MediaType_e media_type) {
    int i = 0;

    SET_IsConnect(media_type, 0);

    usleep(500 * 1000); // ä¿è¯writeä¸åç»§ç»­ï¼åå° wflagé½ç½®ä¸º1ï¼writeçindexOfBufå°è¢«ç½®ä¸º0;

    for (i = 0; i < SHAREBUF_COUNT; i++) {
        gAvshare.bufStreamHead[media_type][i].wflag = 1;
    }

    dpf("disconnect stream %d successfully\n", media_type);

    return 0;
}

// The reader, rtspLive, holds a write lock on its pid file for as long as it runs (already_running()
// in src/rtspLive/main.cpp, also in earlier versions); the kernel releases the lock when the process
// ends, however it ends. A connect flag set while no process holds that lock was left behind by a
// reader that was killed or crashed: it is not trusted, so the live encoders stop. Without the pid
// file nothing can be told and the flag is trusted, as before.
#define READER_LOCKFILE "/tmp/live.pid"
#define READER_CHECK_MS 500

static bool avshare_readerAlive(void) {
    static uint32_t tickChecked;
    static bool checked = false;
    static bool alive = true;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t tickNow = (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000);

    if (checked && ((uint32_t)(tickNow - tickChecked) < READER_CHECK_MS)) {
        return alive;
    }
    checked = true;
    tickChecked = tickNow;

    bool wasAlive = alive;
    int fd = open(READER_LOCKFILE, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct flock fl;
        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        alive = (fcntl(fd, F_GETLK, &fl) != 0) || (fl.l_type != F_UNLCK);
        close(fd);
    } else {
        alive = true;
    }

    if (alive != wasAlive) {
        dpf("stream reader %s\n", alive ? "running again" : "gone without disconnecting (" READER_LOCKFILE " not locked): connect flags ignored");
    }
    return alive;
}

bool avshare_connected(MediaType_e streamType) {
    if (!CHECK_IsConnect(streamType) || !avshare_readerAlive()) {
        int streamIndex = MEDIA_type2index(streamType);
        gAvshare.idxNext[streamIndex] = 0;
        return false;
    }

    return true;
}

void avshare_reset(void) {
    dpf("avshare reset\n");

    // A live encoder restart begins a new H.264 stream with fresh SPS/PPS and
    // an IDR. Discard every chunk from the previous encoder instance so the
    // RTSP reader cannot concatenate the two bitstream epochs.
    // ripples-all: only the video side. The stream audio (live_audio.c) writes the audio side from its
    // own thread through the pause and the resume, and its bitstream has no new epoch: flushing it
    // here threw its queued frames away and raced with that writer (idxNext, slot flags).
    if (gAvshare.bufShare != NULL) {
        const int v = MEDIA_type2index(MEDIA_VIDEO);
        for (int j = 0; j < SHAREBUF_COUNT; j++) {
            gAvshare.bufStreamHead[v][j].wflag = 1;
        }
        gAvshare.idxNext[v] = 0;
    }

#if (SAVE_FILE_TEST)
    if (gAvshare.fileTest != NULL) {
        fclose(gAvshare.fileTest);
        gAvshare.fileTest = NULL;
    }
#endif
}

//----------------------------------------------------------------------------------------------
// Live stream audio (RTSP AAC track)
//
// Unlike avshare_add(), a frame is either queued completely or not at all, so the reader never
// sees a partial audio frame. Slots are published in order (first slot first), each one after
// its contents, so a reader that sees a slot filled also sees its data.
//----------------------------------------------------------------------------------------------
int avshare_addAudioFrame(const uint8_t *data, int len, uint64_t timestampUs) {
    const int mediaIndex = MEDIA_type2index(MEDIA_AUDIO);
    int nSlots;
    int i;

    if (gAvshare.bufShare == NULL)
        return -2;

    if (!CHECK_IsConnect(MEDIA_AUDIO)) {
        gAvshare.idxNext[mediaIndex] = 0;
        return -3;
    }

    if (len <= 0)
        return 0;

    nSlots = (len + (audioStreamSize) - 1) / (audioStreamSize); // the macros are not parenthesized
    if (nSlots > SHAREBUF_COUNT / 2)
        return -4;

    // only the reader frees slots, so slots seen free here stay free until we fill them
    for (i = 0; i < nSlots; i++) {
        int idx = (gAvshare.idxNext[mediaIndex] + i) % SHAREBUF_COUNT;
        if (*(volatile char *)&gAvshare.bufStreamHead[mediaIndex][idx].wflag != 1)
            return 1;
    }
    __sync_synchronize();

    for (i = 0; i < nSlots; i++) {
        int idx = gAvshare.idxNext[mediaIndex];
        ShareStream_t *pStreamHead = &gAvshare.bufStreamHead[mediaIndex][idx];
        int chunk = (len > (audioStreamSize)) ? (audioStreamSize) : len;

        memcpy(gAvshare.streamBuf[mediaIndex][idx], data, chunk);
        pStreamHead->frameType = FRAME_I;
        pStreamHead->mediaType = MEDIA_AUDIO;
        pStreamHead->timestamp = timestampUs;
        pStreamHead->length = chunk;
        if (nSlots == 1)
            pStreamHead->buff_type = BUF_BEG_END;
        else if (i == 0)
            pStreamHead->buff_type = BUF_BEGIN;
        else if (i == nSlots - 1)
            pStreamHead->buff_type = BUF_END;
        else
            pStreamHead->buff_type = BUF_MIDDLE;

        __sync_synchronize();
        *(volatile char *)&pStreamHead->wflag = 0;

        gAvshare.idxNext[mediaIndex] = (idx + 1) % SHAREBUF_COUNT;
        data += chunk;
        len -= chunk;
    }

    return 0;
}
