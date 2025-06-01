/**************************************************************************/
/*  audio_decoder.h                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             EIRTeam.FFmpeg                             */
/*                         https://ph.eirteam.moe                         */
/**************************************************************************/
/* Copyright (c) 2023-present Álex Román (EIRTeam) & contributors.        */
/*                                                                        */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

// Osu inspired ffmpeg decoding code

#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#ifdef GDEXTENSION

// Headers for building as GDExtension plug-in.
#include "gdextension_build/command_queue_mt.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/mutex.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/mutex_lock.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/templates/list.hpp>

using namespace godot;

#else

#include "core/io/file_access.h"
#include "core/templates/command_queue_mt.h"

#endif

#include "ffmpeg_codec.h"
#include "ffmpeg_frame.h"
extern "C" {
#include "libavutil/channel_layout.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
}

#include <thread>

class DecodedAudioFrame : public RefCounted {
	double time;

public:
	PackedFloat32Array sample_data;
	double get_time() const;
	void set_time(double p_time);
	PackedFloat32Array get_sample_data() const;
	DecodedAudioFrame(double p_time) { time = p_time; };
};

class AudioDecoder : public RefCounted {
public:
	enum HardwareAudioDecoder {
		NONE = 0,
		NVDEC = 1,
		INTEL_QUICK_SYNC = 2,
		DXVA2 = 4,
		VDPAU = 8,
		VAAPI = 16,
		ANDROID_MEDIACODEC = 32,
		APPLE_VIDEOTOOLBOX = 64,
		ANY = INT_MAX,
	};
	enum DecoderState {
		READY,
		RUNNING,
		FAULTED,
		END_OF_STREAM,
		STOPPED
	};

private:
	Vector<Ref<DecodedAudioFrame>> decoded_audio_frames;

	Mutex audio_buffer_mutex;

	SwsContext *sws_context = nullptr;
	SwrContext *swr_context = nullptr;
	DecoderState decoder_state = DecoderState::READY;
	mutable CommandQueueMT decoder_commands;
	AVStream *audio_stream = nullptr;
	AVIOContext *io_context = nullptr;
	AVFormatContext *format_context = nullptr;
	AVCodecContext *audio_codec_context = nullptr;
	bool input_opened = false;
	bool hw_decoding_allowed = false;
	double audio_time_base_in_seconds;
	double duration;
	double skip_output_until_time = -1.0;
	SafeFlag skip_current_outputs;
	SafeNumeric<float> last_decoded_frame_time;
	Ref<FileAccess> audio_file;
	String audio_path;
	BitField<HardwareAudioDecoder> target_hw_audio_decoders = HardwareAudioDecoder::ANY;
	std::thread *thread = nullptr;
	SafeFlag thread_abort;

	bool looping = false;

	static int _read_packet_callback(void *p_opaque, uint8_t *p_buf, int p_buf_size);
	static int64_t _stream_seek_callback(void *p_opaque, int64_t p_offset, int p_whence);
	void prepare_decoding();
	void recreate_codec_context();
	static HardwareAudioDecoder from_av_hw_device_type(AVHWDeviceType p_device_type);

	void _seek_command(double p_target_timestamp);
	static void _thread_func(void *userdata);
	void _decode_next_frame(AVPacket *p_packet, AVFrame *p_receive_frame);
	int _send_packet(AVCodecContext *p_codec_context, AVFrame *p_receive_frame, AVPacket *p_packet);
	void _try_disable_hw_decoding(int p_error_code);
	void _read_decoded_audio_frames(AVFrame *p_received_frame);

	AVFrame *_ensure_frame_audio_format(AVFrame *p_frame, AVSampleFormat p_target_audio_format);

public:
	struct AvailableDecoderInfo {
		Ref<FFmpegCodec> codec;
		AVHWDeviceType device_type;
	};
	void seek(double p_time, bool p_wait = false);
	void start_decoding();
	Vector<AvailableDecoderInfo> get_available_decoders(const AVInputFormat *p_format, AVCodecID p_codec_id, BitField<HardwareAudioDecoder> p_target_decoders);
	
	void return_audio_frames(Vector<Ref<DecodedAudioFrame>> p_frames);
	void return_audio_frame(Ref<DecodedAudioFrame> p_frame);
	Vector<Ref<DecodedAudioFrame>> get_decoded_audio_frames();
	DecoderState get_decoder_state() const;
	double get_last_decoded_frame_time() const;
	bool is_running() const;
	double get_duration() const;
	int get_audio_mix_rate() const;
	int get_audio_channel_count() const;

	AudioDecoder(Ref<FileAccess> p_file);
	AudioDecoder(const String &p_path);
	~AudioDecoder();
};

#endif // AUDIO_DECODER_H
