/**************************************************************************/
/*  ffmpeg_audio_stream.cpp                                               */
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

#include "ffmpeg_audio_stream.h"

#ifdef GDEXTENSION
#include "gdextension_build/gdex_print.h"
#endif

#include "tracy_import.h"

float FFmpegAudioStreamPlayback::_get_stream_sampling_rate() {
	return decoder->get_audio_mix_rate();
}

int FFmpegAudioStreamPlayback::_mix_resampled(AudioFrame *p_buffer, int p_frames) {
	ZoneScopedN("update_internal");

	if (!playing) {
		return 0;
	}

	if(stream->length==0.0f){
		stream->length=get_length_internal();
	}
	int pos = 0;
	while(pos!=p_frames){
		p_buffer[pos++] = AudioFrame{0.0f, 0.0f};
	}
	playback_position += 0.1 * 1000.0f;

	if (decoder->get_decoder_state() == AudioDecoder::DecoderState::END_OF_STREAM && available_audio_frames.size() == 0) {
		// if at the end of the stream but our playback enters a valid time region again, a seek operation is required to get the decoder back on track.
		if (playback_position < decoder->get_last_decoded_frame_time()) {
			seek_into_sync();
		} else {
			playing = false;
		}
	}

	double frame_time = get_current_frame_time();

	Ref<DecodedAudioFrame> peek_audio_frame;
	if (available_audio_frames.size() > 0) {
		peek_audio_frame = available_audio_frames[0];
	}

	bool audio_out_of_sync = false;

	if (peek_audio_frame.is_valid()) {
		audio_out_of_sync = Math::abs(playback_position - peek_audio_frame->get_time()) > LENIENCE_BEFORE_SEEK;

		if (looping) {
			audio_out_of_sync &= Math::abs(playback_position - decoder->get_duration() - peek_audio_frame->get_time()) > LENIENCE_BEFORE_SEEK &&
					Math::abs(playback_position + decoder->get_duration() - peek_audio_frame->get_time()) > LENIENCE_BEFORE_SEEK;
		}
	}
	if (audio_out_of_sync) {
		// TODO: seek audio stream individually if it desyncs
		// seek_into_sync();
		playback_position = peek_audio_frame->get_time();
		last_playback_position = playback_position;
	}

	pos = 0;
	bool update_playback_position = false;
	while (available_audio_frames.size() > 0 && check_next_audio_frame_valid(available_audio_frames[0])) {
		ZoneNamedN(__audio_mix, "Audio mix", true);
		Ref<DecodedAudioFrame> audio_frame = available_audio_frames[0];
		last_frame = available_audio_frames[0];
		int sample_count = audio_frame->get_sample_data().size() / decoder->get_audio_channel_count();
#ifdef GDEXTENSION

		while(frame_read_pos<sample_count){
			float frame_read_buffer_left = audio_frame->get_sample_data()[frame_read_pos*2];
			float frame_read_buffer_right = audio_frame->get_sample_data()[frame_read_pos*2+1];
			p_buffer[pos++] = AudioFrame{frame_read_buffer_left, frame_read_buffer_right};
			frame_read_pos++;
			if(pos==p_frames){
				break;
			}
		}
		// playback_position = audio_frame->get_time();
		last_playback_position = audio_frame->get_time();
		update_playback_position = true;
		if(Math::abs(frame_read_pos - sample_count) < 0.1){
			available_audio_frames.pop_front();
			frame_read_pos = 0;
		}
		if(pos==p_frames){
			break;
		}
		
#else
		mix_callback(mix_udata, audio_frame->get_sample_data().ptr(), sample_count);
#endif
	}
	if (available_audio_frames.size() == 0) {
		for (Ref<DecodedAudioFrame> frame : decoder->get_decoded_audio_frames()) {
			available_audio_frames.push_back(frame);
		}
	}

	buffering = decoder->is_running() && available_audio_frames.size() == 0;
	if (frame_time != get_current_frame_time()) {
		frames_processed++;
	}
	while(pos!=p_frames){
		p_buffer[pos++] = AudioFrame{0.0f, 0.0f};
	}
	if(update_playback_position && last_playback_position!=0.0){
		playback_position = last_playback_position;
	}
	return p_frames;
}


void FFmpegAudioStreamPlayback::seek_into_sync() {
	decoder->seek(playback_position);
	available_audio_frames.clear();
}

double FFmpegAudioStreamPlayback::get_current_frame_time() {
	if (last_frame.is_valid()) {
		return last_frame->get_time();
	}
	return 0.0f;
}

bool FFmpegAudioStreamPlayback::check_next_audio_frame_valid(Ref<DecodedAudioFrame> p_decoded_frame) {
	// in the case of looping, we may start a seek back to the beginning but still receive some lingering frames from the end of the last loop. these should be allowed to continue playing.
	if (looping && Math::abs((p_decoded_frame->get_time() - decoder->get_duration()) - playback_position) < LENIENCE_BEFORE_SEEK)
		return true;

	return p_decoded_frame->get_time() <= playback_position && Math::abs(p_decoded_frame->get_time() - playback_position) < LENIENCE_BEFORE_SEEK;
}

void FFmpegAudioStreamPlayback::load(Ref<FileAccess> p_file_access) {
	decoder = Ref<AudioDecoder>(memnew(AudioDecoder(p_file_access)));

	decoder->start_decoding();
}

void FFmpegAudioStreamPlayback::load_from_url(const String &p_path) {
	decoder = Ref<AudioDecoder>(memnew(AudioDecoder(p_path)));

	decoder->start_decoding();
}


void FFmpegAudioStreamPlayback::start_internal(double p_time = 0.0) {
	if (decoder->get_decoder_state() == AudioDecoder::FAULTED) {
		playing = false;
		return;
	}
	clear();
	playback_position = p_time * 1000.0f;
	decoder->seek(playback_position, true);
	playing = true;
}

void FFmpegAudioStreamPlayback::stop_internal() {
	if (playing) {
		clear();
		playback_position = 0.0f;
		decoder->seek(playback_position, true);
	}
	playing = false;
}


bool FFmpegAudioStreamPlayback::is_playing_internal() const {
	return playing;
}

int FFmpegAudioStreamPlayback::get_loop_count_internal() const {
	return loop_count;
}


double FFmpegAudioStreamPlayback::get_playback_position_internal() const {
	return last_playback_position / 1000.0;
}

void FFmpegAudioStreamPlayback::seek_internal(double p_time) {
	decoder->seek(p_time * 1000.0f);
	available_audio_frames.clear();
	playback_position = p_time * 1000.0f;
	frame_read_pos = 0;
}


void FFmpegAudioStreamPlayback::tag_used_streams_internal() {
}


double FFmpegAudioStreamPlayback::get_length_internal() const {
	return decoder->get_duration() / 1000.0f;
}


int FFmpegAudioStreamPlayback::get_mix_rate_internal() const {
	return decoder->get_audio_mix_rate();
}

int FFmpegAudioStreamPlayback::get_channels_internal() const {
	return decoder->get_audio_channel_count();
}

FFmpegAudioStreamPlayback::FFmpegAudioStreamPlayback() {
}

void FFmpegAudioStreamPlayback::clear() {
	last_frame.unref();
	available_audio_frames.clear();
	frames_processed = 0;
	playing = false;
}
