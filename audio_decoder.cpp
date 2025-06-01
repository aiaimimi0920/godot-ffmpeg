/**************************************************************************/
/*  audio_decoder.cpp                                                     */
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

#include "audio_decoder.h"
#include "ffmpeg_frame.h"

#include "tracy_import.h"

#ifdef GDEXTENSION
#include "gdextension_build/gdex_print.h"
#endif

extern "C" {
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
}

const int MAX_PENDING_FRAMES = 5;

String ffmpeg_audio_get_error_message(int p_error_code) {
	const uint64_t buffer_size = 256;
	Vector<char> buffer;
	buffer.resize(buffer_size);

	int str_error_code = av_strerror(p_error_code, buffer.ptrw(), buffer.size());

	if (str_error_code < 0) {
		return vformat("%d (av_strerror failed with code %d)", p_error_code, str_error_code);
	}

	return String::utf8(buffer.ptr());
}

int AudioDecoder::_read_packet_callback(void *p_opaque, uint8_t *p_buf, int p_buf_size) {
	AudioDecoder *decoder = (AudioDecoder *)p_opaque;
	uint64_t read_bytes = decoder->audio_file->get_buffer(p_buf, p_buf_size);
	return read_bytes != 0 ? read_bytes : AVERROR_EOF;
}

int64_t AudioDecoder::_stream_seek_callback(void *p_opaque, int64_t p_offset, int p_whence) {
	AudioDecoder *decoder = (AudioDecoder *)p_opaque;
	switch (p_whence) {
		case SEEK_CUR: {
			decoder->audio_file->seek(decoder->audio_file->get_position() + p_offset);
		} break;
		case SEEK_SET: {
			decoder->audio_file->seek(p_offset);
		} break;
		case SEEK_END: {
			decoder->audio_file->seek_end(p_offset);
		} break;
		case AVSEEK_SIZE: {
			return decoder->audio_file->get_length();
		} break;
		default: {
			return -1;
		} break;
	}
	return decoder->audio_file->get_position();
}

void AudioDecoder::prepare_decoding() {
	int open_input_res;
	if(!audio_file.is_null()){
		const int context_buffer_size = 4096;
		unsigned char *context_buffer = (unsigned char *)av_malloc(context_buffer_size);
		io_context = avio_alloc_context(context_buffer, context_buffer_size, 0, this, &AudioDecoder::_read_packet_callback, nullptr, &AudioDecoder::_stream_seek_callback);

		format_context = avformat_alloc_context();
		format_context->pb = io_context;
		format_context->flags |= AVFMT_FLAG_GENPTS; // required for most HW decoders as they only read `pts`
		AVDictionary* opts = nullptr;
		av_dict_set(&opts, "buffer_size", "655360", 0);
		av_dict_set(&opts, "hwaccel", "auto", 0);
		av_dict_set(&opts, "movflags", "faststart", 0);
		av_dict_set(&opts, "refcounted_frames", "1", 0);
		open_input_res = avformat_open_input(&format_context, "dummy", nullptr, nullptr);
		av_dict_free(&opts);
	}else if (!audio_path.is_empty()){
		avformat_network_init();
		format_context = avformat_alloc_context();
		AVDictionary* opts = nullptr;
		av_dict_set(&opts, "buffer_size", "655360", 0);
		av_dict_set(&opts, "hwaccel", "auto", 0);
		av_dict_set(&opts, "movflags", "faststart", 0);
		av_dict_set(&opts, "refcounted_frames", "1", 0);
		open_input_res = avformat_open_input(&format_context, audio_path.utf8().get_data(), nullptr, &opts);
		av_dict_free(&opts);
	}

	input_opened = open_input_res >= 0;
	ERR_FAIL_COND_MSG(!input_opened, vformat("Error opening file or stream: %s", ffmpeg_audio_get_error_message(open_input_res)));

	int find_stream_info_result = avformat_find_stream_info(format_context, nullptr);
	ERR_FAIL_COND_MSG(find_stream_info_result < 0, vformat("Error finding stream info: %s", ffmpeg_audio_get_error_message(find_stream_info_result)));

	int audio_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	ERR_FAIL_COND_MSG(audio_stream_index < 0, vformat("Couldn't find audio stream: %s", ffmpeg_audio_get_error_message(audio_stream_index)));

	audio_stream = format_context->streams[audio_stream_index];
	audio_time_base_in_seconds = audio_stream->time_base.num / (double)audio_stream->time_base.den;

	if (audio_stream->duration > 0) {
		duration = audio_stream->duration * audio_time_base_in_seconds * 1000.0;
	} else {
		duration = format_context->duration / (double)AV_TIME_BASE * 1000.0;
	}
}

void AudioDecoder::recreate_codec_context() {
	if (audio_stream == nullptr) {
		return;
	}
	AVCodecParameters codec_params = *audio_stream->codecpar;
	const AVCodec *codec = avcodec_find_decoder(codec_params.codec_id);
	if (codec) {
		if (audio_codec_context != nullptr) {
			avcodec_free_context(&audio_codec_context);
		}
		audio_codec_context = avcodec_alloc_context3(codec);
		ERR_FAIL_COND_MSG(audio_codec_context == nullptr, vformat("Couldn't allocate codec context: %s", codec->name));
		audio_codec_context->pkt_timebase = audio_stream->time_base;

		int param_copy_result = avcodec_parameters_to_context(audio_codec_context, audio_stream->codecpar);
		ERR_FAIL_COND_MSG(param_copy_result < 0, vformat("Couldn't copy codec parameters from %s: %s", codec->name, ffmpeg_audio_get_error_message(param_copy_result)));
		int open_codec_result = avcodec_open2(audio_codec_context, codec, nullptr);
		ERR_FAIL_COND_MSG(open_codec_result < 0, vformat("Error trying to open %s codec: %s", codec->name, ffmpeg_audio_get_error_message(open_codec_result)));
		print_line("Succesfully initialized audio decoder:", codec->name);
	}
}

AudioDecoder::HardwareAudioDecoder AudioDecoder::from_av_hw_device_type(AVHWDeviceType p_device_type) {
	switch (p_device_type) {
		case AV_HWDEVICE_TYPE_NONE: {
			return AudioDecoder::NONE;
		} break;
		case AV_HWDEVICE_TYPE_VDPAU: {
			return AudioDecoder::VDPAU;
		} break;
		case AV_HWDEVICE_TYPE_CUDA: {
			return AudioDecoder::NVDEC;
		} break;
		case AV_HWDEVICE_TYPE_VAAPI: {
			return AudioDecoder::VAAPI;
		} break;
		case AV_HWDEVICE_TYPE_DXVA2: {
			return AudioDecoder::DXVA2;
		} break;
		case AV_HWDEVICE_TYPE_QSV: {
			return AudioDecoder::INTEL_QUICK_SYNC;
		} break;
		case AV_HWDEVICE_TYPE_MEDIACODEC: {
			return AudioDecoder::ANDROID_MEDIACODEC;
		} break;
		default: {
		} break;
	}
	return AudioDecoder::NONE;
}

void AudioDecoder::_seek_command(double p_target_timestamp) {
	av_seek_frame(format_context, audio_stream->index, (long)(p_target_timestamp / audio_time_base_in_seconds / 1000.0), AVSEEK_FLAG_BACKWARD);
	// No need to seek the audio stream separately since it is seeked automatically with the audio stream
	// due to being in the same file
	avcodec_flush_buffers(audio_codec_context);
	skip_output_until_time = p_target_timestamp;
	decoder_state = DecoderState::READY;
	skip_current_outputs.clear();
}

void AudioDecoder::_thread_func(void *userdata) {
	AudioDecoder *decoder = (AudioDecoder *)userdata;
	AVPacket *packet = av_packet_alloc();
	AVFrame *receive_frame = av_frame_alloc();

#ifdef GDEXTENSION
	String audio_decoding_str = vformat("Audio decoding %d", OS::get_singleton()->get_thread_caller_id());
#else
	String audio_decoding_str = vformat("Audio decoding %d", Thread::get_caller_id());
#endif
	CharString str = audio_decoding_str.utf8();
	while (!decoder->thread_abort.is_set()) {
		switch (decoder->decoder_state) {
			case READY:
			case RUNNING: {
				decoder->audio_buffer_mutex.lock();
				bool needs_frame = decoder->decoded_audio_frames.size() < MAX_PENDING_FRAMES;
				decoder->audio_buffer_mutex.unlock();
				if (needs_frame) {
					FrameMarkStart(audio_decoding);
					decoder->_decode_next_frame(packet, receive_frame);
					FrameMarkEnd(audio_decoding);
				} else {
					decoder->decoder_state = DecoderState::READY;
					OS::get_singleton()->delay_usec(1000);
				}
			} break;
			case END_OF_STREAM: {
				// While at the end of the stream, avoid attempting to read further as this comes with a non-negligible overhead.
				// A Seek() operation will trigger a state change, allowing decoding to potentially start again.
				OS::get_singleton()->delay_usec(50000);
			} break;
			default: {
				ERR_PRINT("Invalid decoder state");
			} break;
		}
		decoder->decoder_commands.flush_if_pending();
	}

	av_packet_free(&packet);
	av_frame_free(&receive_frame);

	if (decoder->decoder_state != DecoderState::FAULTED) {
		decoder->decoder_state = DecoderState::STOPPED;
	}
}

void AudioDecoder::_decode_next_frame(AVPacket *p_packet, AVFrame *p_receive_frame) {
	ZoneScopedN("Audio decoder decode next frame");
	int read_frame_result = 0;

	if (p_packet->buf == nullptr) {
		read_frame_result = av_read_frame(format_context, p_packet);
	}

	if (read_frame_result >= 0) {
		decoder_state = DecoderState::RUNNING;

		bool unref_packet = true;

		if (p_packet->stream_index == audio_stream->index) {
			AVCodecContext *codec_ctx = audio_codec_context;
			int send_packet_result = _send_packet(codec_ctx, p_receive_frame, p_packet);

			if (send_packet_result == -EAGAIN) {
				unref_packet = false;
			}
		}

		if (unref_packet) {
			av_packet_unref(p_packet);
		}
	} else if (read_frame_result == AVERROR_EOF) {
		_send_packet(audio_codec_context, p_receive_frame, nullptr);
		if (looping) {
			seek(0);
		} else {
			decoder_state = DecoderState::END_OF_STREAM;
		}
	} else if (read_frame_result == -EAGAIN) {
		decoder_state = DecoderState::READY;
		OS::get_singleton()->delay_usec(1000);
	} else {
		print_line(vformat("Failed to read data into avcodec packet: %s", ffmpeg_audio_get_error_message(read_frame_result)));
	}
}

int AudioDecoder::_send_packet(AVCodecContext *p_codec_context, AVFrame *p_receive_frame, AVPacket *p_packet) {
	ZoneScopedN("Audio decoder send packet");
	// send the packet for decoding.
	int send_packet_result;
	{
		ZoneNamedN(__avcodec_send_packet, "avcodec_send_packet", true);
		send_packet_result = avcodec_send_packet(p_codec_context, p_packet);
	}
	// Note: EAGAIN can be returned if there's too many pending frames, which we have to read,
	// otherwise we would get stuck in an infinite loop.
	if (send_packet_result == 0 || send_packet_result == -EAGAIN) {
		if (p_codec_context == audio_codec_context) {
			_read_decoded_audio_frames(p_receive_frame);
		}
	} else if (format_context->streams[p_packet->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
		print_line(vformat("Failed to send avcodec packet: %s", ffmpeg_audio_get_error_message(send_packet_result)));
		_try_disable_hw_decoding(send_packet_result);
	}

	return send_packet_result;
}

void AudioDecoder::_try_disable_hw_decoding(int p_error_code) {
	if (!hw_decoding_allowed || target_hw_audio_decoders == HardwareAudioDecoder::NONE || audio_codec_context == nullptr || audio_codec_context->hw_device_ctx == nullptr) {
		return;
	}

	hw_decoding_allowed = false;

	if (p_error_code == -ENOMEM) {
		print_line("Disabling hardware decoding of audio due to a lack of memory");
		target_hw_audio_decoders = HardwareAudioDecoder::NONE;
	} else {
		print_line("Disabling hardware decoding of the audio due to an unexpected error");
	}
	decoder_commands.push(this, &AudioDecoder::recreate_codec_context);
}

void AudioDecoder::_read_decoded_audio_frames(AVFrame *p_received_frame) {
	Vector<uint8_t> unwrapped_frame;
	while (true) {
		ZoneScopedN("Audio decoder read decoded frame");
		int receive_frame_result = avcodec_receive_frame(audio_codec_context, p_received_frame);

		if (receive_frame_result < 0) {
			if (receive_frame_result != -EAGAIN && receive_frame_result != AVERROR_EOF) {
				print_line(vformat("Failed to receive frame from avcodec: %s", ffmpeg_audio_get_error_message(receive_frame_result)));
				_try_disable_hw_decoding(receive_frame_result);
			}

			break;
		}

		// use `best_effort_timestamp` as it can be more accurate if timestamps from the source file (pts) are broken.
		// but some HW codecs don't set it in which case fallback to `pts`
		int64_t frame_timestamp = p_received_frame->best_effort_timestamp != AV_NOPTS_VALUE ? p_received_frame->best_effort_timestamp : p_received_frame->pts;
		double frame_time = (frame_timestamp - audio_stream->start_time) * audio_time_base_in_seconds * 1000.0;

		if (skip_output_until_time > frame_time || skip_current_outputs.is_set()) {
			continue;
		}
		last_decoded_frame_time.set(frame_time);
		AVFrame *frame = _ensure_frame_audio_format(p_received_frame, AV_SAMPLE_FMT_FLT);

		if (!frame) {
			av_frame_unref(p_received_frame);
			return;
		}
		
		ERR_FAIL_COND_MSG(av_sample_fmt_is_planar((AVSampleFormat)frame->format), "Audio format should never be planar, bug?");

		int data_size = av_samples_get_buffer_size(nullptr, frame->ch_layout.nb_channels, frame->nb_samples, (AVSampleFormat)frame->format, 0);
		Ref<DecodedAudioFrame> audio_frame = memnew(DecodedAudioFrame(frame_time));

		audio_frame->set_time(frame_time);
		audio_frame->sample_data.resize(data_size / sizeof(float));
		memset(audio_frame->sample_data.ptrw(), 0, data_size);
		memcpy(audio_frame->sample_data.ptrw(), frame->data[0], data_size);
		audio_buffer_mutex.lock();
		if (!skip_current_outputs.is_set()) {
			decoded_audio_frames.push_back(audio_frame);
		}
		audio_buffer_mutex.unlock();

		av_frame_unref(p_received_frame);
		if (frame != p_received_frame) {
			av_frame_free(&frame);
		}
	}
}

AVFrame *AudioDecoder::_ensure_frame_audio_format(AVFrame *p_frame, AVSampleFormat p_target_audio_format) {
	ZoneScopedN("Audio decoder rescale");
	if (p_frame->format == p_target_audio_format) {
		return p_frame;
	}

	// This initialization and AV_CHANNEL_LAYOUT_STEREO is equivalent, only because if is used AV_CHANNEL_LAYOUT_STEREO, then we need std20
	AVChannelLayout outChannelLayout;
	outChannelLayout.order = AV_CHANNEL_ORDER_NATIVE;
	outChannelLayout.nb_channels= (2);
	outChannelLayout.u.mask = { AV_CH_LAYOUT_STEREO };
	outChannelLayout.opaque = NULL; 
	int obtain_swr_ctx_result = swr_alloc_set_opts2(
			&swr_context,
			&outChannelLayout, p_target_audio_format, audio_codec_context->sample_rate,
			&audio_codec_context->ch_layout, audio_codec_context->sample_fmt, audio_codec_context->sample_rate,
			0, nullptr);

	if (obtain_swr_ctx_result < 0) {
		print_line("Failed to obtain SWR context");
		return nullptr;
	}
	AVFrame *out_frame;

	out_frame = av_frame_alloc();
	out_frame->format = p_target_audio_format;
	out_frame->ch_layout = outChannelLayout;
	out_frame->sample_rate = audio_codec_context->sample_rate;
	out_frame->nb_samples = p_frame->nb_samples;

	int get_buffer_result = av_frame_get_buffer(out_frame, 0);

	if (get_buffer_result < 0) {
		print_line("Failed to allocate SWR frame buffer:", ffmpeg_audio_get_error_message(get_buffer_result));
		av_frame_unref(out_frame);
		return nullptr;
	}

	int converter_result = swr_convert_frame(swr_context, out_frame, p_frame);

	if (converter_result < 0) {
		print_line("Failed to convert audio frame:", ffmpeg_audio_get_error_message(converter_result));
		av_frame_unref(out_frame);
		return nullptr;
	}

	return out_frame;
}

void AudioDecoder::seek(double p_time, bool p_wait) {
	audio_buffer_mutex.lock();

	decoded_audio_frames.clear();

	last_decoded_frame_time.set(p_time);
	skip_current_outputs.set();
	audio_buffer_mutex.unlock();
	if (p_wait) {
		decoder_commands.push_and_sync(this, &AudioDecoder::_seek_command, p_time);
	} else {
		decoder_commands.push(this, &AudioDecoder::_seek_command, p_time);
	}
}

void AudioDecoder::start_decoding() {
	ERR_FAIL_COND_MSG(thread != nullptr, "Cannot start decoding once already started");
	if (format_context == nullptr) {
		prepare_decoding();
		recreate_codec_context();

		if (audio_stream == nullptr) {
			decoder_state = DecoderState::FAULTED;
			return;
		}
	}

	thread = memnew(std::thread(_thread_func, this));
}

int get_hw_audio_decoder_score(AVHWDeviceType p_device_type) {
	switch (p_device_type) {
		case AV_HWDEVICE_TYPE_VDPAU: {
			return 10;
		} break;
		case AV_HWDEVICE_TYPE_CUDA: {
			return 10;
		} break;
		case AV_HWDEVICE_TYPE_VAAPI: {
			return 9;
		} break;
		case AV_HWDEVICE_TYPE_DXVA2: {
			return 8;
		} break;
		case AV_HWDEVICE_TYPE_QSV: {
			return 9;
		} break;
		case AV_HWDEVICE_TYPE_MEDIACODEC: {
			return 10;
		} break;
		default: {
		} break;
	}
	return INT_MIN;
}

struct AvailableDecoderInfoComparator {
	bool operator()(const AudioDecoder::AvailableDecoderInfo &p_a, const AudioDecoder::AvailableDecoderInfo &p_b) const {
		return get_hw_audio_decoder_score(p_a.device_type) > get_hw_audio_decoder_score(p_b.device_type);
	}
};

Vector<AudioDecoder::AvailableDecoderInfo> AudioDecoder::get_available_decoders(const AVInputFormat *p_format, AVCodecID p_codec_id, BitField<HardwareAudioDecoder> p_target_decoders) {
	Vector<AudioDecoder::AvailableDecoderInfo> codecs;

	Ref<FFmpegCodec> first_codec;

	void *iterator = NULL;
	while (true) {
		const AVCodec *av_codec = av_codec_iterate(&iterator);

		if (av_codec == NULL) {
			break;
		}

		if (av_codec->id != p_codec_id || !av_codec_is_decoder(av_codec)) {
			continue;
		}

		Ref<FFmpegCodec> codec = memnew(FFmpegCodec(av_codec));
		if (!first_codec.is_valid()) {
			first_codec = codec;
		}

		if (p_target_decoders == HardwareAudioDecoder::NONE) {
			break;
		}

		for (AVHWDeviceType type : codec->get_supported_hw_device_types()) {
			HardwareAudioDecoder hw_audio_decoder = from_av_hw_device_type(type);
			if (hw_audio_decoder == NONE || !p_target_decoders.has_flag(hw_audio_decoder)) {
				continue;
			}
			codecs.push_back(AvailableDecoderInfo{
					codec,
					type });
		}
	}

	// default to the first codec that we found with no HW devices.
	// The first codec is what FFmpeg's `avcodec_find_decoder` would return so this way we'll automatically fallback to that.
	if (first_codec.is_valid()) {
		codecs.push_back(AvailableDecoderInfo{
				first_codec,
				AV_HWDEVICE_TYPE_NONE });
	}

	codecs.sort_custom<AvailableDecoderInfoComparator>();
	return codecs;
}

Vector<Ref<DecodedAudioFrame>> AudioDecoder::get_decoded_audio_frames() {
	MutexLock lock(audio_buffer_mutex);
	Vector<Ref<DecodedAudioFrame>> frames = decoded_audio_frames.duplicate();
	decoded_audio_frames.clear();
	return frames;
}

AudioDecoder::DecoderState AudioDecoder::get_decoder_state() const {
	return decoder_state;
}

double AudioDecoder::get_last_decoded_frame_time() const {
	return last_decoded_frame_time.get();
}

bool AudioDecoder::is_running() const {
	return decoder_state == DecoderState::RUNNING;
}

double AudioDecoder::get_duration() const {
	return duration;
}

int AudioDecoder::get_audio_mix_rate() const {
	if (audio_stream) {
		return audio_codec_context->sample_rate;
	}
	return 0;
}

int AudioDecoder::get_audio_channel_count() const {
	if (audio_stream) {
		// return audio_codec_context->ch_layout.nb_channels;
		return 2;
	}
	return 0;
}

void AudioDecoder::return_audio_frames(Vector<Ref<DecodedAudioFrame>> p_frames) {
	for (Ref<DecodedAudioFrame> frame : p_frames) {
		return_audio_frame(frame);
	}
}

void AudioDecoder::return_audio_frame(Ref<DecodedAudioFrame> p_frame) {
	MutexLock lock(audio_buffer_mutex);
	decoded_audio_frames.push_back(p_frame);
}

AudioDecoder::AudioDecoder(Ref<FileAccess> p_file) {
	audio_file = p_file;
}

AudioDecoder::AudioDecoder(const String &p_path) {
	audio_path = p_path;
}


AudioDecoder::~AudioDecoder() {
	if (thread != nullptr) {
		thread_abort.set_to(true);
		thread->join();
		memdelete(thread);
	}

	if (format_context != nullptr && input_opened) {
		avformat_close_input(&format_context);
	}

	if (audio_codec_context != nullptr) {
		avcodec_free_context(&audio_codec_context);
	}

	if (sws_context != nullptr) {
		sws_freeContext(sws_context);
	}

	if (swr_context != nullptr) {
		swr_free(&swr_context);
	}

	if (io_context != nullptr) {
		av_free(io_context->buffer);
		avio_context_free(&io_context);
	}
}

double DecodedAudioFrame::get_time() const {
	return time;
}

void DecodedAudioFrame::set_time(double p_time) { time = p_time; }


PackedFloat32Array DecodedAudioFrame::get_sample_data() const {
	return sample_data;
}