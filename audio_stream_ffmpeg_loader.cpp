/**************************************************************************/
/*  audio_stream_ffmpeg_loader.cpp                                        */
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

#include "audio_stream_ffmpeg_loader.h"
extern "C" {
#include "libavformat/avformat.h"
}
#include "ffmpeg_audio_stream.h"

void AudioStreamFFMpegLoader::_update_recognized_extension_cache() const {
	if (recognized_extension_cache.size() > 0) {
		return;
	}
	void *iteration_state = nullptr;
	const AVInputFormat *current_fmt = nullptr;

	while ((current_fmt = av_demuxer_iterate(&iteration_state)) != nullptr) {
		if (current_fmt->extensions == nullptr) {
			continue;
		}
		PackedStringArray demuxer_exts = String(current_fmt->extensions).split(",", false);
		const_cast<AudioStreamFFMpegLoader *>(this)->recognized_extension_cache.append_array(demuxer_exts);
	}
}

String AudioStreamFFMpegLoader::get_resource_type_internal(const String &p_path) const {
	_update_recognized_extension_cache();
	if (recognized_extension_cache.has(p_path.get_extension())) {
		return "AudioStreamFFMpegLoader";
	}
	return "";
}

Ref<Resource> AudioStreamFFMpegLoader::load_internal(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) const {
    if (std::string::npos != p_path.to_lower().find("//")) {
        
    } else{
		Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
		if (f.is_null()) {
			if (r_error) {
				*r_error = ERR_CANT_OPEN;
			}
			return Ref<Resource>();
		}
	}

	Ref<FFmpegAudioStream> stream;
	stream.instantiate();
	stream->set_file(p_path);

	if (r_error) {
		*r_error = OK;
	}

	return stream;
}
bool AudioStreamFFMpegLoader::handles_type_internal(const String &p_type) const {
#ifdef GDEXTENSION
	return p_type == "AudioStream";
#else
	return ClassDB::is_parent_class(p_type, "AudioStreamFFMpegLoader");
#endif
}

#ifdef GDEXTENSION
PackedStringArray AudioStreamFFMpegLoader::_get_recognized_extensions() const {
	_update_recognized_extension_cache();
	return recognized_extension_cache;
}

bool AudioStreamFFMpegLoader::_handles_type(const StringName &p_type) const {
	return AudioStreamFFMpegLoader::handles_type_internal(p_type);
}

Variant AudioStreamFFMpegLoader::_load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const {
	return load_internal(p_path, p_original_path, nullptr, p_use_sub_threads, nullptr, (CacheMode)p_cache_mode);
}

#else
void AudioStreamFFMpegLoader::get_recognized_extensions(List<String> *p_extensions) const {
	_update_recognized_extension_cache();
	for (String ext : recognized_extension_cache) {
		p_extensions->push_back(ext);
	}
}
Ref<Resource> AudioStreamFFMpegLoader::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	return load_internal(p_path, p_original_path, r_error, p_use_sub_threads, r_progress, p_cache_mode);
}
#endif
