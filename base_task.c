/* 
 * File:    base_task.c
 *           A basic real-time task skeleton, updated to run as a
 *           video processing application using ffmpeg and SDL 
 *           libraries.
 * 
 * Author:  Farhan Rahman sxr190032
 *        	Created on 11/28/2019.
 *
 * References: https://github.com/rambodrahmani/ffmpeg-video-player
 */

/* Include standard headers.
 * Generally speaking, a LITMUS^RT real-time task can perform any
 * system call, etc., but no real-time guarantees can be made if a
 * system call blocks. To be on the safe side, only use I/O for debugging
 * purposes and from non-real-time sections.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <time.h>

/* Include ffmpeg libraries */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>


#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

/* Include the LITMUS^RT user space library header.
 * This header, part of liblitmus, provides the user space API of
 * LITMUS^RT.
 */
#include "litmus.h"

/* Next, we define period and execution cost to be constant. 
 * These are only constants for convenience in this example, they can be
 * determined at run time, e.g., from command line parameters.
 *
 * These are in milliseconds.
 */
#define PERIOD            10
#define RELATIVE_DEADLINE 100
#define EXEC_COST         10

/* Catch errors. */
#define CALL( exp ) do { \
	int ret; \
	ret = exp; \
	if (ret != 0) \
	 fprintf(stderr, "%s failed: %m\n", #exp);\
	else \
	 fprintf(stderr, "%s ok.\n", #exp); \
} while (0)

#define SDL_AUDIO_BUFFER_SIZE 	1024
#define MAX_AUDIO_FRAME_SIZE 	192000

/**
 * Struct used to hold the format context, the indices of the audio and video stream,
 * the corresponding AVStream objects, the audio and video codec information,
 * the audio and video queues and buffers, the global quit flag and the filename of
 * the movie.
 */

typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
} PacketQueue;


/* Initialize ffmpeg variables */
int             	i, videoStream, audioStream;
int             	frameFinished;
AVPacket 			packet;
AVFormatContext 	*pFormatCtx 	= NULL;
AVCodecContext  	*pCodecCtxOrig 	= NULL;
AVCodecContext  	*pCodecCtx 		= NULL;
AVCodec         	*pCodec 		= NULL;
AVFrame         	*pFrame 		= NULL;
struct SwsContext 	*sws_ctx 		= NULL;

AVCodecContext  *aCodecCtxOrig 	= NULL;
AVCodecContext  *aCodecCtx 		= NULL;
AVCodec         *aCodec 		= NULL;

SDL_Overlay     *bmp;
SDL_Surface     *screen;
SDL_Rect        rect;
SDL_Event       event;
SDL_AudioSpec   wanted_spec, spec;

// audio PacketQueue instance
PacketQueue audioq;

// global quit flag
int quit = 0;

/* Declare the periodically invoked job. 
 * Returns 1 -> task should exit.
 *         0 -> task should continue.
 */
int job(void);

/**
 * Initialize the given PacketQueue.
 *
 * @param   q   the PacketQueue to be initialized.
 */
void packet_queue_init(PacketQueue *q) {

	// alloc memory for the audio queue
	memset(q, 0, sizeof(PacketQueue));

	// Returns the initialized and unlocked mutex or NULL on failure
	q->mutex = SDL_CreateMutex();
	if (!q->mutex)
    {
        // could not create mutex
        printf("SDL_CreateMutex Error: %s.\n", SDL_GetError());
        return;
    }

    // Returns a new condition variable or NULL on failure
	q->cond = SDL_CreateCond();
	if (!q->cond)
    {
        // could not create condition variable
        printf("SDL_CreateCond Error: %s.\n", SDL_GetError());
        return;
    }
}

/**
 * Put the given AVPacket in the given PacketQueue.
 *
 * @param 	q   	the queue to be used for the insert
 * @param 	pkt 	the AVPacket to be inserted in the queue
 *
 * @Returns 		0 if the AVPacket is correctly inserted in the given PacketQueue.
 */
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

	AVPacketList *pkt1;

	// if (av_packet_ref(pkt) < 0)
	// 	return -1;

	// alloc the new AVPacketList to be inserted in the audio PacketQueue
	pkt1 = av_malloc(sizeof(AVPacketList));
	if (!pkt1)
		return -1;

	// add reference to the given AVPacket
	pkt1->pkt = *pkt;

	// the new AVPacketList will be inserted at the end of the queue
	pkt1->next = NULL;

	SDL_LockMutex(q->mutex);

	// check the queue is empty
	if (!q->last_pkt){
		// Insert as first
		q->first_pkt = pkt1;
	}
	else {
		// Insert as last
		q->last_pkt->next = pkt1;
	}

	// point the last AVPacketList in the queue to the newly created AVPacketList
	q->last_pkt = pkt1;

	// increase by 1 the number of AVPackets in the queue
	q->nb_packets++;

	// increase queue size by adding the size of the newly inserted AVPacket
	q->size += pkt1->pkt.size;

	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mutex);

	return 0;
}

/**
 * Get the first AVPacket from the given PacketQueue.
 *
 * @param   q       the PacketQueue to extract from
 * @param   pkt     the first AVPacket extracted from the queue
 * @param   block   0 to avoid waiting for an AVPacket to be inserted in the given
 *                  queue, != 0 otherwise.
 *
 * @return          < 0 if returning because the quit flag is set, 
 *					0 if the queue is empty, 
 *					1 if it is not empty and a packet was extract (pkt)
 */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
	AVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for(;;) {
		if(quit) {
		  ret = -1;
		  break;
		}

		// point to the first AVPacketList in the queue
		pkt1 = q->first_pkt;

		// if the first packet is not NULL, the queue is not empty
		if (pkt1) {

			// place the second packet in the queue at first position
			q->first_pkt = pkt1->next;

			// check if queue is empty after removal
			if (!q->first_pkt)
				q->last_pkt = NULL;

			// decrease the number of packets in the queue
			q->nb_packets--;

			// decrease the size of the packets in the queue
			q->size -= pkt1->pkt.size;

			// point pkt to the extracted packet
			*pkt = pkt1->pkt;

			av_free(pkt1);
			ret = 1;
			break;

		} else if (!block) {

			ret = 0;
			break;

		} else {

			SDL_CondWait(q->cond, q->mutex);

		}
	}

	SDL_UnlockMutex(q->mutex);
	return ret;
}


/**
 * Resample the audio data retrieved using FFmpeg before playing it.
 *
 * @param   audio_decode_ctx    the audio codec context retrieved from the original AVFormatContext.
 * @param   decoded_audio_frame the decoded audio frame.
 * @param   out_sample_fmt      audio output sample format (e.g. AV_SAMPLE_FMT_S16).
 * @param   out_channels        audio output channels, retrieved from the original audio codec context.
 * @param   out_sample_rate     audio output sample rate, retrieved from the original audio codec context.
 * @param   out_buf             audio output buffer.
 *
 * @return                      the size of the resampled audio data.
 */
static int audio_resampling(                                  
                        AVCodecContext * audio_decode_ctx,
                        AVFrame * decoded_audio_frame,
                        enum AVSampleFormat out_sample_fmt,
                        int out_channels,
                        int out_sample_rate,
                        uint8_t * out_buf
                      )
{

    SwrContext * swr_ctx = NULL;
    int ret = 0;
    int64_t in_channel_layout = audio_decode_ctx->channel_layout;
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    int out_nb_channels = 0;
    int out_linesize = 0;
    int in_nb_samples = 0;
    int out_nb_samples = 0;
    int max_out_nb_samples = 0;
    uint8_t ** resampled_data = NULL;
    int resampled_data_size = 0;

    // check global quit flag
    if (quit)
    {
        return -1;
    }

    swr_ctx = swr_alloc();

    if (!swr_ctx)
    {
        printf("swr_alloc error.\n");
        return -1;
    }

    // get input audio channels
    in_channel_layout = (audio_decode_ctx->channels ==
                     av_get_channel_layout_nb_channels(audio_decode_ctx->channel_layout)) ?   
                     audio_decode_ctx->channel_layout :
                     av_get_default_channel_layout(audio_decode_ctx->channels);

    // check input audio channels correctly retrieved
    if (in_channel_layout <= 0)
    {
        printf("in_channel_layout error.\n");
        return -1;
    }

    // set output audio channels based on the input audio channels
    if (out_channels == 1)
    {
        out_channel_layout = AV_CH_LAYOUT_MONO;
    }
    else if (out_channels == 2)
    {
        out_channel_layout = AV_CH_LAYOUT_STEREO;
    }
    else
    {
        out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }

    // retrieve number of audio samples (per channel)
    in_nb_samples = decoded_audio_frame->nb_samples;
    if (in_nb_samples <= 0)
    {
        printf("in_nb_samples error.\n");
        return -1;
    }

    // Set SwrContext parameters for resampling
    av_opt_set_int(   
        swr_ctx,
        "in_channel_layout",
        in_channel_layout,
        0
    );

    // Set SwrContext parameters for resampling
    av_opt_set_int(
        swr_ctx,
        "in_sample_rate",
        audio_decode_ctx->sample_rate,
        0
    );

    // Set SwrContext parameters for resampling
    av_opt_set_sample_fmt(
        swr_ctx,
        "in_sample_fmt",
        audio_decode_ctx->sample_fmt,
        0
    );

    // Set SwrContext parameters for resampling
    av_opt_set_int(
        swr_ctx,
        "out_channel_layout",
        out_channel_layout,
        0
    );

    // Set SwrContext parameters for resampling
    av_opt_set_int(
        swr_ctx,
        "out_sample_rate",
        out_sample_rate,
        0
    );

    // Set SwrContext parameters for resampling
    av_opt_set_sample_fmt(
        swr_ctx,
        "out_sample_fmt",
        out_sample_fmt,
        0
    );

    // Once all values have been set for the SwrContext, it must be initialized
    // with swr_init().
    ret = swr_init(swr_ctx);;
    if (ret < 0)
    {
        printf("Failed to initialize the resampling context.\n");
        return -1;
    }

    max_out_nb_samples = out_nb_samples = av_rescale_rnd(
                                              in_nb_samples,
                                              out_sample_rate,
                                              audio_decode_ctx->sample_rate,
                                              AV_ROUND_UP
                                          );

    // check rescaling was successful
    if (max_out_nb_samples <= 0)
    {
        printf("av_rescale_rnd error.\n");
        return -1;
    }

    // get number of output audio channels
    out_nb_channels = av_get_channel_layout_nb_channels(out_channel_layout);

    ret = av_samples_alloc_array_and_samples(
              &resampled_data,
              &out_linesize,
              out_nb_channels,
              out_nb_samples,
              out_sample_fmt,
              0
          );

    if (ret < 0)
    {
        printf("av_samples_alloc_array_and_samples() error: Could not allocate destination samples.\n");
        return -1;
    }

    // retrieve output samples number taking into account the progressive delay
    out_nb_samples = av_rescale_rnd(
                        swr_get_delay(swr_ctx, audio_decode_ctx->sample_rate) + in_nb_samples,
                        out_sample_rate,
                        audio_decode_ctx->sample_rate,
                        AV_ROUND_UP
                     );

    // check output samples number was correctly retrieved
    if (out_nb_samples <= 0)
    {
        printf("av_rescale_rnd error\n");
        return -1;
    }

    if (out_nb_samples > max_out_nb_samples)
    {
        // free memory block and set pointer to NULL
        av_free(resampled_data[0]);

        // Allocate a samples buffer for out_nb_samples samples
        ret = av_samples_alloc(
                  resampled_data,
                  &out_linesize,
                  out_nb_channels,
                  out_nb_samples,
                  out_sample_fmt,
                  1
              );

        // check samples buffer correctly allocated
        if (ret < 0)
        {
            printf("av_samples_alloc failed.\n");
            return -1;
        }

        max_out_nb_samples = out_nb_samples;
    }

    if (swr_ctx)
    {
        // do the actual audio data resampling
        ret = swr_convert(
                  swr_ctx,
                  resampled_data,
                  out_nb_samples,
                  (const uint8_t **) decoded_audio_frame->data,
                  decoded_audio_frame->nb_samples
              );

        // check audio conversion was successful
        if (ret < 0)
        {
            printf("swr_convert_error.\n");
            return -1;
        }

        // Get the required buffer size for the given audio parameters
        resampled_data_size = av_samples_get_buffer_size(
                                  &out_linesize,
                                  out_nb_channels,
                                  ret,
                                  out_sample_fmt,
                                  1
                              );

        // check audio buffer size
        if (resampled_data_size < 0)
        {
            printf("av_samples_get_buffer_size error.\n");
            return -1;
        }
    }
    else
    {
        printf("swr_ctx null error.\n");
        return -1;
    }

    // copy the resampled data to the output buffer
    memcpy(out_buf, resampled_data[0], resampled_data_size);

    /*
     * Memory Cleanup.
     */
    if (resampled_data)
    {
        // free memory block and set pointer to NULL
        av_freep(&resampled_data[0]);
    }

    av_freep(&resampled_data);
    resampled_data = NULL;

    if (swr_ctx)
    {
        // Free the given SwrContext and set the pointer to NULL
        swr_free(&swr_ctx);
    }

    return resampled_data_size;
}

/**
 * Get a packet from the queue if available. Decode the extracted packet. Once
 * we have the frame, resample it and simply copy it to our audio buffer, making
 * sure the data_size is smaller than our audio buffer.
 *
 * @param   aCodecCtx   the audio AVCodecContext used for decoding
 * @param   audio_buf   the audio buffer to write into
 * @param   buf_size    the size of the audio buffer, 1.5 larger than the one
 *                      provided by FFmpeg
 *
 * @return              0 if everything goes well, -1 in case of error or quit
 */
int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {

	int len1 = 0;
	int data_size = 0;

	AVPacket * avPacket = av_packet_alloc();
	//static AVPacket pkt;
	static uint8_t *audio_pkt_data = NULL;
	static int audio_pkt_size = 0;

	// allocate a new frame, used to decode audio packets
	static AVFrame * avFrame = NULL;
	//static AVFrame frame;
    avFrame = av_frame_alloc();
    if (!avFrame)
    {
        printf("Could not allocate AVFrame.\n");
        return -1;
    }

	for (;;) {
		if (quit)
			return -1;

		while(audio_pkt_size > 0) {
			int got_frame = 0;
			//len1 = avcodec_decode_audio4(aCodecCtx, &frame, &got_frame, &pkt);
			int ret = avcodec_receive_frame(aCodecCtx, avFrame);
			if (ret == 0) {
                got_frame = 1;
            }
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
            }
            if (ret == 0) {
                ret = avcodec_send_packet(aCodecCtx, avPacket);
            }
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
            }
            else if (ret < 0) {
                printf("Error while decoding audio.\n");
                return -1;
            }
            else {
                len1 = avPacket->size;
            }
			if (len1 < 0) {
				/* if error, skip frame */
				audio_pkt_size = 0;
				break;
			}
			audio_pkt_data += len1;
			audio_pkt_size -= len1;
			data_size = 0;
			if (got_frame) {
				// data_size = av_samples_get_buffer_size(NULL, 
				// 										aCodecCtx->channels,
				// 										avFrame->nb_samples,
				// 										aCodecCtx->sample_fmt,
				// 										1);
				// audio resampling
                data_size = audio_resampling(
                                aCodecCtx,
                                avFrame,
                                AV_SAMPLE_FMT_S16,
                                aCodecCtx->channels,
                                aCodecCtx->sample_rate,
                                audio_buf
                            );
				assert(data_size <= buf_size);
				//memcpy(audio_buf, avFrame->data[0], data_size);
			}
			if (data_size <= 0) {
				/* No data yet, get more frames */
				continue;
			}
			/* We have data, return it and come back for more later */
			return data_size;
		}
		if (avPacket->data)
			av_packet_unref(avPacket);

		// get more audio AVPacket
		if (packet_queue_get(&audioq, avPacket, 1) < 0) {
			printf("Error while getting more audio AVPacket.\n");
			return -1;
		}
		
		audio_pkt_data = avPacket->data;
		audio_pkt_size = avPacket->size;
	}
}

/**
 * Pull in data from audio_decode_frame(), store the result in an intermediary
 * buffer, attempt to write as many bytes as the amount defined by len to SDL
 * stream, and get more data if we don't have enough yet, or save it for later
 * if we have some left over.
 *
 * @param   userdata    the pointer we gave to SDL.
 * @param   stream      the buffer we will be writing audio data to.
 * @param   len         the size of that buffer.
 */
void audio_callback(void *userdata, Uint8 *stream, int len) {

	int len1, audio_size;

	// The size of audio_buf is 1.5 times the size of the largest audio frame
    // that FFmpeg will give us, which gives us a nice cushion.
	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	// retrieve the audio codec context
	aCodecCtx = (AVCodecContext *)userdata;

	while (len > 0) {

		if (quit)
			return;

		if (audio_buf_index >= audio_buf_size) {

			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
			if (audio_size < 0) {

				/* If error, output silence */
				audio_buf_size = 1024; 

				/* Clear memory */
				memset(audio_buf, 0, audio_buf_size);

				printf("audio_decode_frame() failed.\n");

			} else {
				audio_buf_size = audio_size;
			}

			audio_buf_index = 0;
		}

		len1 = audio_buf_size - audio_buf_index;
		if (len1 > len)
			len1 = len;

		// copy data from audio buffer to the SDL stream
		memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);

		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
}

/* typically, main() does a couple of things: 
 * 	1) parse command line parameters, etc.
 *	2) Setup work environment.
 *	3) Setup real-time parameters.
 *	4) Transition to real-time mode.
 *	5) Invoke periodic or sporadic jobs.
 *	6) Transition to background mode.
 *	7) Clean up and exit.
 *
 * The following main() function provides the basic skeleton of a single-threaded
 * LITMUS^RT real-time task. In a real program, all the return values should be 
 * checked for errors.
 */

int main(int argc, char** argv)
{
	int do_exit, ret;
	struct rt_task param;

	/* Setup task parameters */
	init_rt_task_param(&param);
	param.exec_cost = ms2ns(EXEC_COST);
	param.period = ms2ns(PERIOD);
	param.relative_deadline = ms2ns(RELATIVE_DEADLINE);

	/* What to do in the case of budget overruns? */
	param.budget_policy = NO_ENFORCEMENT;

	/* The task class parameter is ignored by most plugins. */
	param.cls = RT_CLASS_SOFT;

	/* The priority parameter is only used by fixed-priority plugins. */
	param.priority = LITMUS_LOWEST_PRIORITY;

 	// Register all formats and codecs
  	// av_register_all();
  
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		exit(1);
	}

	// Open video file
	if(avformat_open_input(&pFormatCtx, "/home/farhan/Videos/small.mp4", NULL, NULL) != 0){
        printf("Couldn't open video file\n");
		return -1; 
    }
  
	// Retrieve stream information
	if(avformat_find_stream_info(pFormatCtx, NULL) < 0){
        printf("Couldn't find stream information\n");
		return -1; 
    }
  
	// Dump information about file onto standard error
	av_dump_format(pFormatCtx, 0, "/home/farhan/Videos/small.mp4", 0);

	// Find the first video stream
	videoStream = -1;
	audioStream = -1;
	for (int i = 0; i < pFormatCtx->nb_streams; i++) {
		if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
			videoStream = i;
		}
	  	if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO && audioStream < 0) {
			audioStream = i;
		}
	}
	
	if(videoStream==-1){
		printf("No Video Stream!");
		return -1; // Didn't find a video stream
	}
	if(audioStream==-1){
		printf("No Audio Stream!");
		return -1; // Didn't find a audio stream
	}

	// Retrieve audio codec

	//aCodecCtxOrig=pFormatCtx->streams[audioStream]->codecpar;
	aCodec = avcodec_find_decoder(pFormatCtx->streams[audioStream]->codecpar->codec_id);
	if(aCodec == NULL) {
		fprintf(stderr, "Unsupported audio codec!\n");
		return -1;
	}

	// Copy context
	aCodecCtxOrig = avcodec_alloc_context3(aCodec);
	ret = avcodec_parameters_to_context(aCodecCtxOrig, pFormatCtx->streams[videoStream]->codecpar);
	if (ret != 0) {
			printf("Could not copy audio codec context.\n");
			return -1;
	}

	aCodecCtx = avcodec_alloc_context3(aCodec);
	// if(avcodec_copy_context(aCodecCtx, aCodecCtxOrig) != 0) {
	// 	fprintf(stderr, "Couldn't copy codec context");
	// 	return -1; // Error copying codec context
	// }
	ret = avcodec_parameters_to_context(aCodecCtx, pFormatCtx->streams[audioStream]->codecpar);
	if (ret != 0) {
        printf("Could not copy audio codec context.\n");
        return -1;
    }

	// Set audio settings from codec info
	wanted_spec.freq = aCodecCtx->sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = aCodecCtx->channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = aCodecCtx;

	if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
		fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
		return -1;
	}

	// initialize the audio AVCodecContext to use the given audio AVCodec
	if(avcodec_open2(aCodecCtx, aCodec, NULL)){
	 	printf("Could not open audio codec.\n");
		return -1;
	}

	packet_queue_init(&audioq);

	// start playing audio on the given audio device
	SDL_PauseAudio(0);

	// Retrieve video codec

	// Find the decoder for the video stream
	pCodec=avcodec_find_decoder(pFormatCtx->streams[videoStream]->codecpar->codec_id);
	if(pCodec==NULL) {
		fprintf(stderr, "Unsupported video codec!\n");
		return -1; // Codec not found
	}

	// Copy context
	//pCodecCtxOrig=pFormatCtx->streams[videoStream]->codecpar;
	pCodecCtxOrig = avcodec_alloc_context3(pCodec);
	ret = avcodec_parameters_to_context(pCodecCtxOrig, pFormatCtx->streams[videoStream]->codecpar);
	if (ret != 0) {
        printf("Could not copy video codec context.\n");
        return -1;
    }

	pCodecCtx = avcodec_alloc_context3(pCodec);
	// if(avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
	// 	fprintf(stderr, "Couldn't copy codec context");
	// 	return -1; // Error copying codec context
	// }
	ret = avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoStream]->codecpar);
	if (ret != 0) {
        printf("Could not copy video codec context.\n");
        return -1;
    }

	// initialize the video AVCodecContext to use the given video AVCodec
	if(avcodec_open2(pCodecCtx, pCodec, NULL)<0)
		return -1; // Could not open codec
  
	// Allocate video frame
	pFrame=av_frame_alloc();

	// Make a screen to put our video
	#ifndef __DARWIN__
	  screen = SDL_SetVideoMode(pCodecCtx->width, pCodecCtx->height, 0, 0);
	#else
	  screen = SDL_SetVideoMode(pCodecCtx->width, pCodecCtx->height, 24, 0);
	#endif
	if(!screen) {
		fprintf(stderr, "SDL: could not set video mode - exiting\n");
		exit(1);
	}
  
	// Allocate a place to put our YUV image on that screen
	bmp = SDL_CreateYUVOverlay(pCodecCtx->width,
								pCodecCtx->height,
								SDL_YV12_OVERLAY,
								screen);

	// initialize SWS context for software scaling
	sws_ctx = sws_getContext(pCodecCtx->width,
								pCodecCtx->height,
								pCodecCtx->pix_fmt,
								pCodecCtx->width,
								pCodecCtx->height,
								AV_PIX_FMT_YUV420P,
								SWS_BILINEAR,
								NULL,
								NULL,
								NULL
								); 	

	/* The task is in background mode upon startup. */


	/*****
	 * 1) Command line paramter parsing would be done here.
	 */



	/*****
	 * 2) Work environment (e.g., global data structures, file data, etc.) would
	 *    be setup here.
	 */



	/*****
	 * 3) Setup real-time parameters. 
	 *    In this example, we create a sporadic task that does not specify a 
	 *    target partition (and thus is intended to run under global scheduling). 
	 *    If this were to execute under a partitioned scheduler, it would be assigned
	 *    to the first partition (since partitioning is performed offline).
	 */
	CALL( init_litmus() );

	/* To specify a partition, do
	 *
	 * param.cpu = CPU;
	 * be_migrate_to(CPU);
	 *
	 * where CPU ranges from 0 to "Number of CPUs" - 1 before calling
	 * set_rt_task_param().
	 */
  	CALL( set_rt_task_param(gettid(), &param) );

  	fprintf(stderr, "%s\n", "Set to Real Time Task...");

	/*****
	 * 4) Transition to real-time mode.
	 */
  	CALL( task_mode(LITMUS_RT_TASK) );

	/* The task is now executing as a real-time task if the call didn't fail. 
	 */

  	fprintf(stderr,"%s\n","Running RT task...");
	/*****
	 * 5) Invoke real-time jobs.
	 */
  	do {
		/* Wait until the next job is released. */
    	sleep_next_period();
		/* Invoke job. */
    	do_exit = job();		
		// printf("%d",do_exit);
  	} while (!do_exit);



	/*****
	 * 6) Transition to background mode.
	 */
	  fprintf(stderr,"%s\n","Completed task successfully...");
	  fprintf(stderr,"%s\n","Changing to background task...");
	  CALL( task_mode(BACKGROUND_TASK) );



	/***** 
	 * 7) Clean up, maybe print results and stats, and exit.
	 */
	// Free the packet that was allocated by av_read_frame
	    // Free the YUV frame
	fprintf(stderr,"%s\n","Cleaning...");
	av_frame_free(&pFrame);
  
	// Close the codecs
	avcodec_close(pCodecCtxOrig);
	avcodec_close(pCodecCtx);
	avcodec_close(aCodecCtxOrig);
	avcodec_close(aCodecCtx);

	// Close the video file
	avformat_close_input(&pFormatCtx);
	fprintf(stderr,"%s\n","Cleaning done...");
	fprintf(stderr,"%s\n","Program ending...");

	return 0;
}



int job(void) 
{	
	AVFrame pict;
	int ret;

	// read data from the AVFormatContext by calling av_read_frame()
	if(av_read_frame(pFormatCtx, &packet) >= 0) {

	    // video stream found
	    if(packet.stream_index==videoStream) {

			// Decode video frame
			//avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);

			// give the decoder raw compressed data in an AVPacket
	        ret = avcodec_send_packet(pCodecCtx, &packet);
	        if (ret < 0) {
	            printf("Error sending packet for decoding.\n");
	            return -1;
	        }

	        while (ret >= 0) {
	            // get decoded output data from decoder
	            ret = avcodec_receive_frame(pCodecCtx, pFrame);

	            // check an entire frame was decoded
	            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
	            {
	                break;
	            }
	            else if (ret < 0)
	            {
	                printf("Error while decoding video.\n");
	                return -1;
	            }
	            else
	            {
	                frameFinished = 1;
	            }

	            // Did we get a video frame?
				if(frameFinished) {
					SDL_LockYUVOverlay(bmp);

					pict.data[0] = bmp->pixels[0];
					pict.data[1] = bmp->pixels[2];
					pict.data[2] = bmp->pixels[1];

					pict.linesize[0] = bmp->pitches[0];
					pict.linesize[1] = bmp->pitches[2];
					pict.linesize[2] = bmp->pitches[1];

					// Convert the image into YUV format that SDL uses	
					sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data,
								pFrame->linesize, 0, pCodecCtx->height,
								pict.data, pict.linesize);

					SDL_UnlockYUVOverlay(bmp);

					rect.x = 0;
					rect.y = 0;
					rect.w = pCodecCtx->width;
					rect.h = pCodecCtx->height;
					SDL_DisplayYUVOverlay(bmp, &rect);
					av_packet_unref(&packet);
				}
	        }

			
	    } else if(packet.stream_index==audioStream) {
			packet_queue_put(&audioq, &packet);
	    } else {
	      	av_packet_unref(&packet);
	    }

	   	// handle quit event (Ctrl + C, SDL Window closed)
	    SDL_PollEvent(&event);
	    switch(event.type) {
		    case SDL_QUIT:
				quit = 1;
				SDL_Quit();
				exit(0);
				break;
		    default:
		      	break;
    	}
		
		return 0;

	} else {
	  	return 1;
	}

}

