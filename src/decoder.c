/** References
 * https://itnext.io/build-ffmpeg-webassembly-version-ffmpeg-js-part-2-compile-with-emscripten-4c581e8c9a16
 * https://steemit.com/programming/@targodan/decoding-audio-files-with-ffmpeg
**/

#include <stdio.h>

#include <emscripten.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#define C_CAST(type, variable) ((type)variable)
#define REINTERPRET_CAST(type, variable) C_CAST(type, variable)
#define STATIC_CAST(type, variable) C_CAST(type, variable)
#define RAW_OUT_ON_PLANAR 0

FILE **outFiles;

void done(int code, const char *msg) {
    EM_ASM({
      meta['Exit Message'] = UTF8ToString($1);
      meta['Exit Code'] = $0;
    }, code, msg);

    if (code != 0) {
      exit(code);
    }
}

/**
 * Extract a single sample and convert to float.
 */
float getSample(const AVCodecContext* codecCtx, uint8_t* buffer, int sampleIndex) {
    int64_t val = 0;
    float ret = 0;
    int sampleSize = av_get_bytes_per_sample(codecCtx->sample_fmt);
    switch(sampleSize) {
        case 1:
            // 8bit samples are always unsigned
            val = REINTERPRET_CAST(uint8_t*, buffer)[sampleIndex];
            // make signed
            val -= 127;
            break;

        case 2:
            val = REINTERPRET_CAST(int16_t*, buffer)[sampleIndex];
            break;

        case 4:
            val = REINTERPRET_CAST(int32_t*, buffer)[sampleIndex];
            break;

        case 8:
            val = REINTERPRET_CAST(int64_t*, buffer)[sampleIndex];
            break;

        default:
            done(8, "invalid sample size");
            return 0;
    }

    // Check which data type is in the sample.
    switch(codecCtx->sample_fmt) {
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_U8P:
        case AV_SAMPLE_FMT_S16P:
        case AV_SAMPLE_FMT_S32P:
            // integer => Scale to [-1, 1] and convert to float.
            ret = val / STATIC_CAST(float, ((1 << (sampleSize*8-1))-1));
            break;

        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
            // float => reinterpret
            ret = *REINTERPRET_CAST(float*, &val);
            break;

        case AV_SAMPLE_FMT_DBL:
        case AV_SAMPLE_FMT_DBLP:
            // double => reinterpret and then static cast down
            ret = STATIC_CAST(float, *REINTERPRET_CAST(double*, &val));
            break;

        default:
            done(9, "invalid sample format");
            return 0;
    }

    return ret;
}

/**
 * Write the frame to an output file.
 */
void handleFrame(const AVCodecContext* codecCtx, const AVFrame* frame) {
    if(av_sample_fmt_is_planar(codecCtx->sample_fmt) == 1) {
        // This means that the data of each channel is in its own buffer.
        // => frame->extended_data[i] contains data for the i-th channel.
        for(int s = 0; s < frame->nb_samples; ++s) {
            for(int c = 0; c < codecCtx->channels; ++c) {
                float sample = getSample(codecCtx, frame->extended_data[c], s);
                fwrite(&sample, sizeof(float), 1, outFiles[c]);
            }
        }
    } else {
        // This means that the data of each channel is in the same buffer.
        // => frame->extended_data[0] contains data of all channels.
        if(RAW_OUT_ON_PLANAR) {
            fwrite(frame->extended_data[0], 1, frame->linesize[0], outFiles[0]);
        } else {
            for(int s = 0; s < frame->nb_samples; ++s) {
                for(int c = 0; c < codecCtx->channels; ++c) {
                    float sample = getSample(codecCtx, frame->extended_data[0], s*codecCtx->channels+c);
                    fwrite(&sample, sizeof(float), 1, outFiles[c]);
                }
            }
        }
    }
}

/**
 * Find the first audio stream and returns its index. If there is no audio stream returns -1.
 */
int findAudioStream(const AVFormatContext* formatCtx) {
    int audioStreamIndex = -1;
    for(size_t i = 0; i < formatCtx->nb_streams; ++i) {
        // Use the first audio stream we can find.
        // NOTE: There may be more than one, depending on the file.
        if(formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = i;
            break;
        }
    }
    return audioStreamIndex;
}

/*
 * Print information about the input file and the used codec.
 */
void printStreamInformation(const AVCodec* codec, const AVCodecContext* codecCtx, int audioStreamIndex) {
    EM_ASM({
      meta['Codec Name'] = UTF8ToString($0);
    }, codec->long_name);
    EM_ASM({
      meta['Stream Index'] = $0;
    }, audioStreamIndex);
    EM_ASM({
      meta['Sample Format'] = UTF8ToString($0);
    }, av_get_sample_fmt_name(codecCtx->sample_fmt));
    EM_ASM({
      meta['Sample Rate'] = $0;
    }, codecCtx->sample_rate);
    EM_ASM({
      meta['Sample Size'] = $0;
    }, av_get_bytes_per_sample(codecCtx->sample_fmt));
    EM_ASM({
      meta['Channels'] = $0;
    }, codecCtx->channels);
}

/**
 * Receive as many frames as available and handle them.
 */
int receiveAndHandle(AVCodecContext* codecCtx, AVFrame* frame) {
    int err = 0;
    // Read the packets from the decoder.
    // NOTE: Each packet may generate more than one frame, depending on the codec.
    while((err = avcodec_receive_frame(codecCtx, frame)) == 0) {
        // Let's handle the frame in a function.
        handleFrame(codecCtx, frame);
        // Free any buffers and reset the fields to default values.
        av_frame_unref(frame);
    }
    return err;
}

/*
 * Drain any buffered frames.
 */
void drainDecoder(AVCodecContext* codecCtx, AVFrame* frame) {
    int err = 0;
    // Some codecs may buffer frames. Sending NULL activates drain-mode.
    if((err = avcodec_send_packet(codecCtx, NULL)) == 0) {
        // Read the remaining packets from the decoder.
        err = receiveAndHandle(codecCtx, frame);
        if(err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
            // Neither EAGAIN nor EOF => Something went wrong.
            done(10, "receive error");
        }
    } else {
        // Something went wrong.
        done(11, "send error");
    }
}

int decoder(const char *filename) {
    EM_ASM({
      meta['LIBAVCODEC Version'] = $0 + '.' + $1 + '.' + $2;
    }, LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);
    EM_ASM({
      meta['LIBAVFORMAT Version'] = $0 + '.' + $1 + '.' + $2;
    }, LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO);

    int err = 0;
    AVFormatContext *formatCtx = NULL;
    // Open the file and read the header.
    if ((err = avformat_open_input(&formatCtx, filename, NULL, 0)) != 0) {
        done(1, "cannot open input file");
    }

    // In case the file had no header, read some frames and find out which format and codecs are used.
    // This does not consume any data. Any read packets are buffered for later use.
    avformat_find_stream_info(formatCtx, NULL);

    // Try to find an audio stream.
    int audioStreamIndex = findAudioStream(formatCtx);
    if(audioStreamIndex == -1) {
        avformat_close_input(&formatCtx);
        avformat_free_context(formatCtx);
        done(2, "none of the available streams are audio streams");
    }

    // Find the correct decoder for the codec.
    AVCodec* codec = avcodec_find_decoder(formatCtx->streams[audioStreamIndex]->codecpar->codec_id);
    if (codec == NULL) {
        avformat_close_input(&formatCtx);
        avformat_free_context(formatCtx);
        done(3, "the codec is not supported");
    }

    // Initialize codec context for the decoder.
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (codecCtx == NULL) {
        // Something went wrong. Cleaning up...
        avformat_close_input(&formatCtx);
        avformat_free_context(formatCtx);
        done(4, "could not allocate a decoding context");
    }

    // Fill the codecCtx with the parameters of the codec used in the read file.
    if ((err = avcodec_parameters_to_context(codecCtx, formatCtx->streams[audioStreamIndex]->codecpar)) != 0) {
        // Something went wrong. Cleaning up...
        avcodec_close(codecCtx);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        avformat_free_context(formatCtx);
        done(5, "cannot set codec context parameters");
    }

    // Explicitly request non planar data.
    codecCtx->request_sample_fmt = av_get_alt_sample_fmt(codecCtx->sample_fmt, 0);

    // Initialize the decoder.
    if ((err = avcodec_open2(codecCtx, codec, NULL)) != 0) {
        avcodec_close(codecCtx);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        avformat_free_context(formatCtx);
        done(6, "cannot initialize the decoder");
    }

    // Print some intersting file information.
    printStreamInformation(codec, codecCtx, audioStreamIndex);

    // initialize output files; each file stores PCM data of its channel
    outFiles =  malloc(codecCtx->channels * sizeof(FILE *));
    for(int c = 0; c < codecCtx->channels; ++c) {
        char name[20];
        sprintf(name, "channel_%d", c);
        outFiles[c] = fopen(name, "w+");
    }

    AVFrame* frame = NULL;
    if ((frame = av_frame_alloc()) == NULL) {
        avcodec_close(codecCtx);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        avformat_free_context(formatCtx);
        done(7, "cannot allocate frame");
    }

    // Prepare the packet.
    AVPacket packet;
    // Set default values.
    av_init_packet(&packet);

    while ((err = av_read_frame(formatCtx, &packet)) != AVERROR_EOF) {
        if(err != 0) {
            break; // Don't return, so we can clean up nicely.
        }
        // Does the packet belong to the correct stream?
        if(packet.stream_index != audioStreamIndex) {
            // Free the buffers used by the frame and reset all fields.
            av_packet_unref(&packet);
            continue;
        }
        // We have a valid packet => send it to the decoder.
        if((err = avcodec_send_packet(codecCtx, &packet)) == 0) {
            // The packet was sent successfully. We don't need it anymore.
            // => Free the buffers used by the frame and reset all fields.
            av_packet_unref(&packet);
        }
        else {
            // Something went wrong.
            // EAGAIN is technically no error here but if it occurs we would need to buffer
            // the packet and send it again after receiving more frames. Thus we handle it as an error here.
            break; // Don't return, so we can clean up nicely.
        }

        // Receive and handle frames.
        // EAGAIN means we need to send before receiving again. So thats not an error.
        if((err = receiveAndHandle(codecCtx, frame)) != AVERROR(EAGAIN)) {
            // Not EAGAIN => Something went wrong.
            break; // Don't return, so we can clean up nicely.
        }
    }
    if (err == AVERROR_EOF) {
      err = 0;
    }

    // Drain the decoder.
    drainDecoder(codecCtx, frame);

    // Free all data used by the frame.
    av_frame_free(&frame);

    // Close the context and free all data associated to it, but not the context itself and free the context itself.
    avcodec_close(codecCtx);
    avcodec_free_context(&codecCtx);

    // We are done here. Close the input.
    avformat_close_input(&formatCtx);
    avformat_free_context(formatCtx);

    // Close the outfile.
    for (int i = 0; i < codecCtx->channels; i++) {
        fclose(outFiles[i]);
    }

    done(err, "NA");
    return 0;
}
