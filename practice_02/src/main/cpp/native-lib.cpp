//
// Created by 叶亮 on 2019/1/2.
//
#include <jni.h>
#include <string>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#define  LOGEW(...) __android_log_print(ANDROID_LOG_WARN, "test",__VA_ARGS__)


extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavcodec/jni.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}


//当前时间戳 clock
long long GetNowMs()
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    int sec = tv.tv_sec%360000;
    long long t = sec*1000+tv.tv_usec/1000;
    return t;
}

//用于硬解码
extern "C"
JNIEXPORT
jint JNI_OnLoad(JavaVM *vm,void *res)
{
    av_jni_set_java_vm(vm,0);
    return JNI_VERSION_1_4;
}

//pts 显示时间   dts解码时间

static double r2d(AVRational r) {
    return r.num == 0 || r.den == 0 ? (double) 0 : r.num / (double) r.den;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_pracitce_videoplay_PlayView_open(JNIEnv *env, jobject instance, jstring url_,
                                          jobject surface) {
    const char *path = env->GetStringUTFChars(url_, 0);

    //1 初始化解封装
    av_register_all();

    AVFormatContext *ic = NULL;

    //2 打开文件
    int re = avformat_open_input(&ic, path, 0, 0);

    if (re != 0) {
        LOGEW("avformat_open_input %s success!", path);
    } else {
        LOGEW("avformat_open_input failed!: %s", av_err2str(re));
    }

    //3 获取流信息
    re = avformat_find_stream_info(ic, 0);
    if (re != 0) {
        LOGEW("avformat_find_stream_info failed!");
    }
    LOGEW("duration = %lld nb_streams = %d", ic->duration, ic->nb_streams);

    int fps = 0;
    int videoStream = 0;
    int audioStream = 1;

    //4 获取视频音频流位置
    for (int i = 0; i < ic->nb_streams; i++) {
        AVStream *as = ic->streams[i];
        if (as->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            LOGEW("视频数据");
            videoStream = i;
            fps = r2d(as->avg_frame_rate);

            LOGEW("fps = %d, width = %d height = %d codeid = %d pixformat = %d", fps,
                  as->codecpar->width,
                  as->codecpar->height,
                  as->codecpar->codec_id,
                  as->codecpar->format);

        } else if (as->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            LOGEW("音频数据");
            audioStream = i;
            LOGEW("sample_rate = %d channels = %d sample_format = %d",
                  as->codecpar->sample_rate,
                  as->codecpar->channels,
                  as->codecpar->format);
        }
    }

    //5 获取音频流信息 和上面遍历取出视音频的流信息是一样的，这种方式更直接
    audioStream = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    LOGEW("av_find_best_stream audioStream %d", audioStream);

    //==================================== 视频解码器 ================================================
    //1 软解码器
    AVCodec *vcodec = avcodec_find_decoder(ic->streams[videoStream]->codecpar->codec_id);

    //硬解码
    //AVCodec *vcodec = avcodec_find_decoder_by_name("h264_mediacodec");

    if (!vcodec) {
        LOGEW("avcodec_find failed");
    }

    //2 解码器初始化
    AVCodecContext *vc = avcodec_alloc_context3(vcodec);

    //3 解码器参数赋值
    avcodec_parameters_to_context(vc, ic->streams[videoStream]->codecpar);

    vc->thread_count = 8;

    //4 打开解码器
    re = avcodec_open2(vc, 0, 0);
    LOGEW("vc timebase = %d/ %d", vc->time_base.num, vc->time_base.den);

    if (re != 0) {
        LOGEW("avcodec_open2 video failed!");
    }

    //==================================== 音频解码器 ================================================
    //1 软解码器
    AVCodec *acodec = avcodec_find_decoder(ic->streams[audioStream]->codecpar->codec_id);

    if (!acodec) {
        LOGEW("avcodec_find failed!");
    }

    //2 解码器初始化
    AVCodecContext *ac = avcodec_alloc_context3(acodec);
    avcodec_parameters_to_context(ac, ic->streams[audioStream]->codecpar);
    ac->thread_count = 1;

    //3 打开解码器
    re = avcodec_open2(ac, 0, 0);
    if (re != 0) {
        LOGEW("avcodec_open2 audio failed!");
    }


    //==================================== 开始解码 ================================================

    //1 定义帧数据
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    //用于测试性能
    long long start = GetNowMs();
    int frameCount = 0;

    //2 像素格式转换的上下文
    SwsContext *vctx = NULL;

    int outwWidth = 1280;
    int outHeight = 720;
    char *rgb = new char[1920*1080*4];
    char *pcm = new char[48000*4*2];

    //3 音频重采样上下文初始化
    SwrContext *actx = swr_alloc();
    actx = swr_alloc_set_opts(actx,
                              av_get_default_channel_layout(2),
                              AV_SAMPLE_FMT_S16,
                              ac->sample_rate,
                              av_get_default_channel_layout(ac->channels),
                              ac->sample_fmt,ac->sample_rate,0,0);

    re = swr_init(actx);
    if(re != 0)
    {
        LOGEW("swr_init failed!");
    }else
    {
        LOGEW("swr_init success!");
    }

    //4 显示窗口初始化
    ANativeWindow *nwin = ANativeWindow_fromSurface(env,surface);
    ANativeWindow_setBuffersGeometry(nwin,outwWidth,outHeight,WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer wbuf;

    for (;;) {

        //这里是测试每秒解码的帧数  每三秒解码多少帧
        if(GetNowMs() - start >= 3000)
        {
            LOGEW("now decode fps is %d", frameCount/3);
            start = GetNowMs();
            frameCount = 0;
        }

        int re = av_read_frame(ic, pkt);
        if (re != 0) {
            LOGEW("读取到结尾处！");
            int pos = 20 * r2d(ic->streams[videoStream]->time_base);
            av_seek_frame(ic, videoStream, pos, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME);
            break;
        }

        AVCodecContext *cc = vc;
        if (pkt->stream_index == audioStream) {
            cc = ac;
        }

        //1 发送到线程中解码
        re = avcodec_send_packet(cc, pkt);

        //清理
        int p = pkt->pts;
        av_packet_unref(pkt);

        if (re != 0) {
            LOGEW("avcodec_send_packet failed!");
            continue;
        }

        //每一帧可能对应多个帧数据，所以要遍历取
        for (;;) {
            //2 解帧数据
            re = avcodec_receive_frame(cc, frame);
            if (re != 0) {
                break;
            }
            //LOGEW("avcodec_receive_frame %lld", frame->pts);

            //如果是视频帧
            if(cc == vc){
                frameCount++;

                //3 初始化像素格式转换的上下文
                vctx = sws_getCachedContext(vctx,
                                            frame->width,
                                            frame->height,
                                            (AVPixelFormat)frame->format,
                                            outwWidth,
                                            outHeight,
                                            AV_PIX_FMT_RGBA,
                                            SWS_FAST_BILINEAR,
                                            0,0,0);

                if(!vctx){
                    LOGEW("sws_getCachedContext failed!");
                }else
                {
                    uint8_t  *data[AV_NUM_DATA_POINTERS] = {0};
                    data[0] = (uint8_t *)rgb;
                    int lines[AV_NUM_DATA_POINTERS] = {0};
                    lines[0] = outwWidth * 4;
                    int h = sws_scale(vctx,
                                      (const uint8_t **)frame->data,
                                      frame->linesize,
                                      0,
                                      frame->height,
                                      data,
                                      lines);
                    LOGEW("sws_scale = %d",h);

                    if(h > 0)
                    {
                        ANativeWindow_lock(nwin,&wbuf,0);
                        uint8_t  *dst = (uint8_t*)wbuf.bits;
                        memcpy(dst, rgb, outwWidth*outHeight*4);
                        ANativeWindow_unlockAndPost(nwin);
                    }
                }

            }else //音频帧
            {
                uint8_t  *out[2] = {0};
                out[0] = (uint8_t*)pcm;

                //音频重采样
                int len = swr_convert(actx,out,frame->nb_samples,(const uint8_t**)frame->data,frame->nb_samples);
                LOGEW("swr_convert = %d", len);
            }
        }
    }

    delete rgb;
    delete pcm;

    //关闭上下文
    avformat_close_input(&ic);
    env->ReleaseStringUTFChars(url_, path);
}



//log===================
//单线程解码
/*
W/test: now decode fps is 18
W/test: now decode fps is 18
W/test: now decode fps is 19
W/test: now decode fps is 19
W/test: now decode fps is 19
W/test: now decode fps is 19*/
// 6线程解码

/*  W/test: now decode fps is 105  */

// 硬解码

/*W/test: now decode fps is 95
W/test: now decode fps is 92  */