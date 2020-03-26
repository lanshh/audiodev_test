#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>
#include "codec_test.h"
#include "audio_hw.h"
#include "alsa_audio.h"
#include "audio-base.h"
#include "tinyalsa/include/tinyalsa/asoundlib.h"

#define NOTIFY_AUDIO_PATH "/data/codectest.pcm"
#define REC_DUR 3 //the unit is second

#define DBG 1
#if DBG
#define LOGINFO(args...) printf(args)
#define LOGERR(args...) fprintf(stderr, args)
#else
#define LOGINFO(args...)
#define LOGERR(args...)
#endif

static struct testcase_info  *tc_info = NULL;

struct pcm_config pcm_config = {
    .channels = 2,
    .rate = 44100,
    .period_size = 512,
    .period_count = 6,
    .format = PCM_FORMAT_S16_LE,
    //.flag = HW_PARAMS_FLAG_LPCM,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = 44100,
#ifdef SPEEX_DENOISE_ENABLE
    .period_size = 1024,
#else
    .period_size = 256,
#endif
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
    //.flag = HW_PARAMS_FLAG_LPCM,
};
struct audio_device g_adev = {0};
struct stream_out g_strout = {.dev =&g_adev };
struct stream_in g_strin = {.dev =&g_adev };

static int maxRecPcm = 0;
static int maxRecPcmPeriod = 0;
static int nTime = 0;
static int fexit = 0;

void set_exit(int exit)
{
    LOGINFO("set_exit %d\n", exit);
    fexit = exit;
}
static void calcAndDispRecAudioStrenth(short *pcm, int len)
{	
    short i, data;

    // calc mac rec value
    for(i = 0; i < len/2; i++) {
        data = abs(*pcm++);
        if(maxRecPcmPeriod < data) {
            maxRecPcmPeriod = data;
        }
    }
    if(nTime++ >= 10) {
        nTime = 0;
        maxRecPcm = maxRecPcmPeriod;
        maxRecPcmPeriod = 0;
    }
}
/* 
 * first capture then play
 */
int rec_play_test_async()
{
    struct audio_device *adev;
    struct stream_out *out;
    struct stream_in *in;
    unsigned bufsize = 0;
    int headsetState = 0;
    unsigned isNeedChangeRate = 0;
    int out_card = -1, in_card = -1;
    int out_device = -1, in_device = -1;
    int id_out = 0;
    int id_in = 0;
    int ret = -1;
    FILE *fp = NULL;
    int durationbytes;
    int recLen = 4*REC_DUR*44100;
    char *recData = NULL;

    adev = &g_adev;
    out = &g_strout;
    in = &g_strin;
    /* read sound card info */
    adev_open_init(adev);
    /* read wired status */
    adev_wired_init(adev);
    out->config = &pcm_config;
    in->config =&pcm_config_in;
    out_card = adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].card;
    out_device = adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].device;
    in_card = adev->dev_in[SND_IN_SOUND_CARD_MIC].card;
    in_device = adev->dev_in[SND_IN_SOUND_CARD_MIC].device;
    if (adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].info && 0 ==
        strncmp("RKRK616", adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].info->cid, 7)) {
        fprintf(stderr, "sound card is RK616, audio capture uses rate change.\n");
        isNeedChangeRate = 1;
    }
    /* default in & out */
    in->device = AUDIO_DEVICE_IN_BUILTIN_MIC;
    out->device = AUDIO_DEVICE_OUT_SPEAKER;
    if (adev->mHeadsetState & BIT_HEADSET) {
        LOGINFO("headset is in\n");
        headsetState = 1;
        in->device = HANDS_FREE_MIC_CAPTURE_ROUTE;
        out->device = AUDIO_DEVICE_OUT_WIRED_HEADSET;
    } else if (adev->mHeadsetState & BIT_HEADSET_NO_MIC) {
        LOGINFO("headset without mic is in\n");
        headsetState = 1;
        out->device = AUDIO_DEVICE_OUT_WIRED_HEADSET;
    }
    if (adev->mHeadsetState & BIT_HDMI_AUDIO) {
        LOGINFO("HDMI is in\n");
        out->device = AUDIO_DEVICE_OUT_AUX_DIGITAL;
    }
    if(route_card_init(&adev->route, out_card)) {
        fprintf(stderr, "rec_play_test_2:route_card_init fail\n");
    }   
    route_pcm_card_open(&adev->route, in_card, getRouteFromDevice(in->device | AUDIO_DEVICE_BIT_IN));
    route_pcm_card_open(&adev->route, out_card, getRouteFromDevice(out->device));
    out->pcm[id_out] = pcm_open(out_card, out_device, PCM_OUT | PCM_MONOTONIC, out->config);
    if (out->pcm[id_out] && !pcm_is_ready(out->pcm[id_out])) {
        fprintf(stderr, "pcm_open() failed: %s, card number = %d", pcm_get_error(out->pcm[id_out]), out_card);
        pcm_close(out->pcm[id_out]);
        ret = -EIO;
        goto fail;
    }
    in->pcm[id_in] = pcm_open(in_card, in_device, PCM_IN, in->config);
    if (in->pcm[id_in] && !pcm_is_ready(in->pcm[id_in])) {
        fprintf(stderr, "pcm_open() failed: %scard number = %d", pcm_get_error(in->pcm[id_in]), in_card);
        pcm_close(in->pcm[id_in]);
        ret = -EIO;
        goto fail;
    }
    fp = fopen(NOTIFY_AUDIO_PATH, "rb");
    if(NULL == fp) {
        fprintf(stderr,"could not open %s file, will go to fail\n", NOTIFY_AUDIO_PATH);
        goto fail;
    }
    recData = (char *)malloc(recLen);
    if (!recData) {
        LOGERR("codec_test_async: malloc memory fail recData\n");
        ret = -ENOMEM;
        goto fail;
    }
    durationbytes = recLen;
    do {
        int dataLen = 0;
        char *data = NULL;

        fseek(fp,0,SEEK_SET);
        bufsize = pcm_get_buffer_size(out->pcm[id_out]);
        usleep(10000);
        while(bufsize == fread(recData, 1, bufsize, fp)) {
            if (pcm_write(out->pcm[id_out], recData, bufsize)) {
                LOGERR("the pcmOut could not write %d bytes file data, will go to fail\n", bufsize);
                goto fail;
            }
        }
        /* pcm read */
        usleep(10000);
        bufsize = pcm_get_buffer_size(in->pcm[id_in]);
        data = recData;
        dataLen = 0;
        do {
            int left = 0;
            int len = 0;

            left = durationbytes - dataLen;
            if (left <= 0 ) {
                LOGERR("pcm_read: %d complete, break\n", dataLen);
                break;
            }
            len = left > bufsize ? bufsize : left;
            if(pcm_read(in->pcm[id_in], data, len)) {
                if (dataLen > bufsize) {
                    LOGERR("pcm_read: %d, fail(>%d) break\n", dataLen, bufsize);
                    break;
                } else {
                    LOGERR("pcm_read: %d, fail, exit\n", dataLen);
                    goto fail;
                }
            }
            data += len;
            dataLen += len;
        } while (1);
        /* pcm write */
        usleep(10000);
        bufsize = pcm_get_buffer_size(out->pcm[id_out]);
        data = recData;
        dataLen = 0;
        do {
            int left = 0;
            int len = 0;

            left = durationbytes - dataLen;
            if (left <= 0 ) {
                LOGERR("pcm_write: %d complete, break\n", dataLen);
                break;
            }
            len = left > bufsize ? bufsize : left;
            if(pcm_write(out->pcm[id_out], data, len)) {
                if (dataLen > bufsize) {
                    LOGERR("pcm_write: %d, fail(>%d) break\n", dataLen, bufsize);
                    break;
                } else {
                    LOGERR("pcm_write: %d, fail, exit\n", dataLen);
                    goto fail;
                }
            }
            data += len;
            dataLen += len;
        } while (1);
        LOGINFO("test finish without error, repeat now\n");
    } while(!fexit);
    ret = 0;
fail:
    LOGINFO("rec_play_test_async exit %d\n", ret);
    if (in->pcm[id_in]) {
        pcm_close(in->pcm[id_in]);
        in->pcm[id_in] = NULL;
    }
    if (out->pcm[id_out]) {
        pcm_close(out->pcm[id_out]);
        out->pcm[id_out] = NULL;
    }
    if (fp){
        fclose(fp);
    }
    route_pcm_close(adev->route, PLAYBACK_OFF_ROUTE);
    route_pcm_close(adev->route, PLAYBACK_OFF_ROUTE);
    if (recData) {
        free(recData);
    }
    return ret;
}

/* 
 * paly sync with capture
 */
int rec_play_test_sync()
{	
    struct audio_device *adev;
    struct stream_out *out;
    struct stream_in *in;
    unsigned bufsize = 0;;
    char *data = NULL;
    int headsetState = 0;
    unsigned isNeedChangeRate = 0;
    int out_card = -1, in_card = -1;
    int out_device = -1, in_device = -1;
    int id_out = 0;
    int id_in = 0;
    int ret = -1;

    adev = &g_adev;
    out = &g_strout;
    in = &g_strin;
    /* read sound card info */
    adev_open_init(adev);
    /* read wired status */
    adev_wired_init(adev);
    out->config = &pcm_config;
    in->config =&pcm_config_in;
    out_card = adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].card;
    out_device = adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].device;
    in_card = adev->dev_in[SND_IN_SOUND_CARD_MIC].card;
    in_device = adev->dev_in[SND_IN_SOUND_CARD_MIC].device;
    if (adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].info && 0 ==
        strncmp("RKRK616", adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].info->cid, 7)) {
        fprintf(stderr, "sound card is RK616, audio capture uses rate change.\n");
        isNeedChangeRate = 1;
    }
    /* default in & out */
    in->device = AUDIO_DEVICE_IN_BUILTIN_MIC;
    out->device = AUDIO_DEVICE_OUT_SPEAKER;
    if (adev->mHeadsetState & BIT_HEADSET) {
        LOGINFO("headset is in\n");
        headsetState = 1;
        in->device = HANDS_FREE_MIC_CAPTURE_ROUTE;
        out->device = AUDIO_DEVICE_OUT_WIRED_HEADSET;
    } else if (adev->mHeadsetState & BIT_HEADSET_NO_MIC) {
        LOGINFO("headset without mic is in\n");
        headsetState = 1;
        out->device = AUDIO_DEVICE_OUT_WIRED_HEADSET;
    }
    if (adev->mHeadsetState & BIT_HDMI_AUDIO) {
        LOGINFO("HDMI is in\n");
        out->device = AUDIO_DEVICE_OUT_AUX_DIGITAL;
    }
    if(route_card_init(&adev->route, out_card)) {
        fprintf(stderr, "rec_play_test_2:route_card_init fail\n");
    }   
    route_pcm_card_open(&adev->route, in_card, getRouteFromDevice(in->device | AUDIO_DEVICE_BIT_IN));
    route_pcm_card_open(&adev->route, out_card, getRouteFromDevice(out->device));

    out->pcm[id_out] = pcm_open(out_card, out_device, PCM_OUT | PCM_MONOTONIC, out->config);
    if (out->pcm[id_out] && !pcm_is_ready(out->pcm[id_out])) {
        fprintf(stderr, "pcm_open() failed: %s, card number = %d", pcm_get_error(out->pcm[id_out]), out_card);
        pcm_close(out->pcm[id_out]);
        ret = -EIO;
        goto fail;
    }
    in->pcm[id_in] = pcm_open(in_card, in_device, PCM_IN, in->config);
    if (in->pcm[id_in] && !pcm_is_ready(in->pcm[id_in])) {
        fprintf(stderr, "pcm_open() failed: %scard number = %d", pcm_get_error(in->pcm[id_in]), in_card);
        pcm_close(in->pcm[id_in]);
        ret = -EIO;
        goto fail;
    }
    bufsize = pcm_get_buffer_size(in->pcm[id_in]);
    data = (char *)malloc(bufsize);
    printf("alloc buffer %p with %d bytes\n", data, bufsize);
    if (!data) {
        fprintf(stderr, "could not allocate %d bytes\n", bufsize);
        ret = -ENOMEM;
        goto fail;
    }
    while (!fexit && !pcm_read(in->pcm[id_in], data, bufsize)) {
        calcAndDispRecAudioStrenth((short *)data, bufsize);
        if (pcm_write(out->pcm[id_out], data, bufsize)) {
            fprintf(stderr, "could not write %d bytes\n", bufsize);
            ret = -EIO;
            goto fail;
        }
    }
    ret = 0;
fail:
    LOGINFO("rec_play_test_sync exit %d\n", ret);
    if (in->pcm[id_in]) {
        pcm_close(in->pcm[id_in]);
        in->pcm[id_in] = NULL;
    }
    if (out->pcm[id_out]) {
        pcm_close(out->pcm[id_out]);
        out->pcm[id_out] = NULL;
    }
    route_pcm_close(adev->route, PLAYBACK_OFF_ROUTE);
    route_pcm_close(adev->route, PLAYBACK_OFF_ROUTE);
    if(data) {
        free(data);
    }
    return 0;
}
