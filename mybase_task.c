/* based_task.c -- A basic real-time task skeleton. 
 *
 * This (by itself useless) task demos how to setup a 
 * single-threaded LITMUS^RT real-time task.
 */

/* First, we include standard headers.
 * Generally speaking, a LITMUS^RT real-time task can perform any
 * system call, etc., but no real-time guarantees can be made if a
 * system call blocks. To be on the safe side, only use I/O for debugging
 * purposes and from non-real-time sections.
 */


/* Include ffmpeg libraries */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <time.h>

/* Second, we include the LITMUS^RT user space library header.
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

/* Catch errors.
 */
#define CALL( exp ) do { \
	int ret; \
	ret = exp; \
	if (ret != 0) \
	 fprintf(stderr, "%s failed: %m\n", #exp);\
	else \
	 fprintf(stderr, "%s ok.\n", #exp); \
} while (0)

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 	1024
#define MAX_AUDIO_FRAME_SIZE 	192000

typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
} PacketQueue;

/**
 * Struct used to hold the format context, the indices of the audio and video stream,
 * the corresponding AVStream objects, the audio and video codec information,
 * the audio and video queues and buffers, the global quit flag and the filename of
 * the movie.
 */

//Initialize ffmpeg variables
int             	i, videoStream, audioStream;
int             	frameFinished;
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

AVPicture pict;
AVPacket packet;

PacketQueue audioq;

int quit = 0;


/* Declare the periodically invoked job. 
 * Returns 1 -> task should exit.
 *         0 -> task should continue.
 */
int job(void);

void packet_queue_init(PacketQueue *q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	if (!q->mutex)
    {
        // could not create mutex
        printf("SDL_CreateMutex Error: %s.\n", SDL_GetError());
        return;
    }
	q->cond = SDL_CreateCond();
	if (!q->cond)
    {
        // could not create condition variable
        printf("SDL_CreateCond Error: %s.\n", SDL_GetError());
        return;
    }
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

	AVPacketList *pkt1;
	// if (av_packet_ref(pkt) < 0)
	// 	return -1;
	pkt1 = av_malloc(sizeof(AVPacketList));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	SDL_LockMutex(q->mutex);

	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else 
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);
	return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
	AVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for(;;) {
		if(quit) {
		  ret = -1;
		  break;
		}

		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;

			if (!q->first_pkt)
				q->last_pkt = NULL;

			q->nb_packets--;
			q->size -= pkt1->pkt.size;
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

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {

	int len1 = 0;
	int data_size = 0;

	//static AVPacket pkt;
	AVPacket * avPacket = av_packet_alloc();
	static uint8_t *audio_pkt_data = NULL;
	static int audio_pkt_size = 0;
	//static AVFrame frame;
	static AVFrame * avFrame = NULL;
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
				data_size = av_samples_get_buffer_size(NULL, 
														aCodecCtx->channels,
														avFrame->nb_samples,
														aCodecCtx->sample_fmt,
														1);
				assert(data_size <= buf_size);
				memcpy(audio_buf, avFrame->data[0], data_size);
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

		if (packet_queue_get(&audioq, avPacket, 1) < 0) {
			return -1;
		}
		
		audio_pkt_data = avPacket->data;
		audio_pkt_size = avPacket->size;
	}
}

void audio_callback(void *userdata, Uint8 *stream, int len) {

	int len1, audio_size;
	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	aCodecCtx = (AVCodecContext *)userdata;

	while (len > 0) {
	if (audio_buf_index >= audio_buf_size) {
		/* We have already sent all our data; get more */
		audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
		if (audio_size < 0) {
			/* If error, output silence */
			audio_buf_size = 1024; // arbitrary?
			memset(audio_buf, 0, audio_buf_size);
		} else {
			audio_buf_size = audio_size;
		}
		audio_buf_index = 0;
	}
	len1 = audio_buf_size - audio_buf_index;
	if (len1 > len)
		len1 = len;
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
	if(avformat_open_input(&pFormatCtx, "/home/farhan/Downloads/videoplayback_360.mp4", NULL, NULL)!=0)
		return -1; // Couldn't open file
  
	// Retrieve stream information
	if(avformat_find_stream_info(pFormatCtx, NULL)<0)
		return -1; // Couldn't find stream information
  
	// Dump information about file onto standard error
	av_dump_format(pFormatCtx, 0, "/home/farhan/Downloads/videoplayback_360.mp4", 0);

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

	//aCodecCtxOrig=pFormatCtx->streams[audioStream]->codecpar;
	aCodec = avcodec_find_decoder(pFormatCtx->streams[audioStream]->codecpar->codec_id);
	if(aCodec == NULL) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}

	// Copy context
	aCodecCtxOrig = avcodec_alloc_context3(aCodec);
	ret = avcodec_parameters_to_context(aCodecCtxOrig, pFormatCtx->streams[videoStream]->codecpar);
	if (ret != 0) {
			printf("Could not copy codec context.\n");
			return -1;
	}

	aCodecCtx = avcodec_alloc_context3(aCodec);
	// if(avcodec_copy_context(aCodecCtx, aCodecCtxOrig) != 0) {
	// 	fprintf(stderr, "Couldn't copy codec context");
	// 	return -1; // Error copying codec context
	// }
	ret = avcodec_parameters_to_context(aCodecCtx, pFormatCtx->streams[audioStream]->codecpar);
	if (ret != 0) {
        printf("Could not copy codec context.\n");
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

	if(avcodec_open2(aCodecCtx, aCodec, NULL)){
	 	printf("Could not open audio codec.\n");
		return -1;
	}

	// audio_st = pFormatCtx->streams[index]
	packet_queue_init(&audioq);
	SDL_PauseAudio(0);

	// Get a pointer to the codec context for the video stream
	//pCodecCtxOrig=pFormatCtx->streams[videoStream]->codecpar;

	// Find the decoder for the video stream
	pCodec=avcodec_find_decoder(pFormatCtx->streams[videoStream]->codecpar->codec_id);
	if(pCodec==NULL) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1; // Codec not found
	}

	// Copy context
	pCodecCtxOrig = avcodec_alloc_context3(pCodec);
	ret = avcodec_parameters_to_context(pCodecCtxOrig, pFormatCtx->streams[videoStream]->codecpar);
	if (ret != 0) {
        printf("Could not copy codec context.\n");
        return -1;
    }

	pCodecCtx = avcodec_alloc_context3(pCodec);
	// if(avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
	// 	fprintf(stderr, "Couldn't copy codec context");
	// 	return -1; // Error copying codec context
	// }
	ret = avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoStream]->codecpar);
	if (ret != 0) {
        printf("Could not copy codec context.\n");
        return -1;
    }

	// Open codec
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
	struct timespec tim, tim2;
	AVFrame pict;
	int ret;

	if(av_read_frame(pFormatCtx, &packet)>=0) {
	    // Is this a packet from the video stream?
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
	   	tim.tv_sec = 0;
	   	tim.tv_nsec = 5000;

	   	if (nanosleep(&tim , &tim2) < 0 ) {
	      	// printf("Nano sleep system call failed \n");
	      	return -1;
	   	}
	    // Free the packet that was allocated by av_read_frame
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

