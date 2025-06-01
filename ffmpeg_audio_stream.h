/**************************************************************************/
/*  ffmpeg_audio_stream.h                                                 */
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

#ifndef ET_AUDIO_STREAM_H
#define ET_AUDIO_STREAM_H

#ifdef GDEXTENSION

// Headers for building as GDExtension plug-in.
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/audio_stream_playback_resampled.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

#else

#include "core/object/ref_counted.h"
#include "servers/audio/audio_stream.h"

#endif

#include "audio_decoder.h"

// We have to use this function redirection system for GDExtension because the naming conventions
// for the functions we are supposed to override are different there

#include "gdextension_build/func_redirect.h"
class FFmpegAudioStreamPlayback : public AudioStreamPlaybackResampled {
	GDCLASS(FFmpegAudioStreamPlayback, AudioStreamPlaybackResampled);
	const int LENIENCE_BEFORE_SEEK = 2500;
	double playback_position = 0.0f;
	double last_playback_position = 0.0f;

	Ref<AudioDecoder> decoder;
	List<Ref<DecodedAudioFrame>> available_audio_frames;
	Ref<DecodedAudioFrame> last_frame;
	int frame_read_pos = 0;
	int frame_read_len = 0;
	bool looping = false;
	bool buffering = false;
	int frames_processed = 0;
	void seek_into_sync();
	double get_current_frame_time();
	bool check_next_audio_frame_valid(Ref<DecodedAudioFrame> p_decoded_frame);
	bool playing = false;
	int loop_count = 0;

	friend class FFmpegAudioStream;
	Ref<FFmpegAudioStream> stream;

private:
	void start_internal(double p_time);
	void stop_internal();
	bool is_playing_internal() const;
	int get_loop_count_internal() const;
	double get_playback_position_internal() const;
	void seek_internal(double p_time);
	void tag_used_streams_internal();

	double get_length_internal() const;
	int get_mix_rate_internal() const;
	int get_channels_internal() const;

	void update_internal(double p_delta);

protected:
	void clear();

	static void _bind_methods(){
		ClassDB::bind_method(D_METHOD("get_length"), &FFmpegAudioStreamPlayback::get_length_internal);
		ClassDB::bind_method(D_METHOD("get_mix_rate"), &FFmpegAudioStreamPlayback::get_mix_rate_internal);
		ClassDB::bind_method(D_METHOD("get_channels"), &FFmpegAudioStreamPlayback::get_channels_internal);
	}; // Required by GDExtension, do not remove

public:
	void load(Ref<FileAccess> p_file_access);
	void load_from_url(const String &p_path);
	
	STREAM_FUNC_REDIRECT_1(void, start, double, p_time);
	STREAM_FUNC_REDIRECT_0(void, stop);
	STREAM_FUNC_REDIRECT_0_CONST(bool, is_playing);
	STREAM_FUNC_REDIRECT_0_CONST(int, get_loop_count);
	STREAM_FUNC_REDIRECT_0_CONST(double, get_playback_position);
	STREAM_FUNC_REDIRECT_1(void, seek, double, p_time);
	STREAM_FUNC_REDIRECT_0(void, tag_used_streams);

	// int _mix(AudioFrame *p_buffer, float p_rate_scale, int p_frames);
	FFmpegAudioStreamPlayback();
	int _mix_resampled(AudioFrame *p_buffer, int p_frames);
	float _get_stream_sampling_rate();
	
};

class FFmpegAudioStream : public AudioStream {
	GDCLASS(FFmpegAudioStream, AudioStream);

protected:
	String file;
	
	static void _bind_methods(){
		ClassDB::bind_method(D_METHOD("set_file", "file"), &FFmpegAudioStream::set_file);
		ClassDB::bind_method(D_METHOD("get_file"), &FFmpegAudioStream::get_file);

		ADD_PROPERTY(PropertyInfo(Variant::STRING, "file"), "set_file", "get_file");

	}; // Required by GDExtension, do not remove

public:
	double length=0.0f;
	void set_file(const String &p_file) {
		file = p_file;
		emit_changed();
	}

	String get_file() {
		return file;
	}

	double _get_length(){
		return length;
	}

	Ref<AudioStreamPlayback> _instantiate_playback() {
		String file_path = get_file();
		if(file_path.to_lower().begins_with("http://") || file_path.to_lower().begins_with("https://")){
			Ref<FFmpegAudioStreamPlayback> pb;
			
			pb.instantiate();
			pb->stream = Ref<FFmpegAudioStream>(this);
			pb->load_from_url(file_path);
			return pb;
		}else{
			Ref<FileAccess> fa = FileAccess::open(file_path, FileAccess::READ);
			if (!fa.is_valid()) {
				return Ref<AudioStreamPlayback>();
			}
			Ref<FFmpegAudioStreamPlayback> pb;
			pb.instantiate();
			pb->stream = Ref<FFmpegAudioStream>(this);
			pb->load(fa);
			return pb;
		}
	}

	// STREAM_FUNC_REDIRECT_0(Ref<AudioStreamPlayback>, instantiate_playback);
};

#endif // ET_AUDIO_STREAM_H
