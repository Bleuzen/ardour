/*
 * Copyright (C) 2023 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2023 Paul Davis <paul@linuxaudiosystems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "ardour/amp.h"
#include "ardour/audio_buffer.h"
#include "ardour/lv2_plugin.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/surround_pannable.h"
#include "ardour/surround_return.h"
#include "ardour/surround_send.h"
#include "ardour/uri_map.h"
#include "pbd/i18n.h"

using namespace ARDOUR;

SurroundReturn::OutputFormatControl::OutputFormatControl (bool v, std::string const& n, PBD::Controllable::Flag f)
	: MPControl<bool> (v, n, f)
{
}

std::string
SurroundReturn::OutputFormatControl::get_user_string () const
{
	if (get_value () == 0) {
		return "7.1.4";
	} else {
		return "5.1";
	}
}

SurroundReturn::BinauralRenderControl::BinauralRenderControl (bool v, std::string const& n, PBD::Controllable::Flag f)
	: MPControl<bool> (v, n, f)
{
}

std::string
SurroundReturn::BinauralRenderControl::get_user_string () const
{
	if (get_value () == 0) {
		return "Dolby";
	} else {
		return "Apple";
	}
}

SurroundReturn::SurroundReturn (Session& s, Route* r)
	: Processor (s, _("SurrReturn"), Temporal::TimeDomainProvider (Temporal::AudioTime))
	, _lufs_meter (s.nominal_sample_rate (), 5)
	, _output_format_control (new OutputFormatControl (false, _("Output Format"), PBD::Controllable::Toggle))
	, _binaural_render_control (new BinauralRenderControl (false, _("Binaural Renderer"), PBD::Controllable::Toggle))
#ifdef __APPLE__
	, _au (0)
	, _au_buffers (0)
	, _au_samples_processed (0)
#endif
	, _have_au_renderer (false)
	, _current_n_objects (max_object_id)
	, _current_output_format (OUTPUT_FORMAT_7_1_4)
	, _in_map (ChanCount (DataType::AUDIO, 128))
	, _out_map (ChanCount (DataType::AUDIO, 14 + 6 /* Loudness Meter */))
	, _exporting (false)
	, _export_start (0)
	, _export_end (0)
{
#if !(defined(LV2_EXTENDED) && defined(HAVE_LV2_1_10_0))
	throw failed_constructor ();
#endif

	_surround_processor = std::dynamic_pointer_cast<LV2Plugin> (find_plugin (_session, "urn:ardour:a-vapor", ARDOUR::LV2));

	if (!_surround_processor) {
		throw ProcessorException (_("Required Atmos/Vapor Processor not found."));
	}

	ChanCount cca128 (ChanCount (DataType::AUDIO, 128));

	_flush.store (0);
	_surround_processor->activate ();
	_surround_bufs.ensure_buffers (DataType::AUDIO, 128, s.get_block_size ());
	_surround_bufs.set_count (cca128);

	lv2_atom_forge_init (&_forge, URIMap::instance ().urid_map ());

	_trim.reset (new Amp (_session, X_("Trim"), r->trim_control (), false));
	_trim->configure_io (cca128, cca128);
	_trim->activate ();

	for (size_t i = 0; i < max_object_id; ++i) {
		_current_render_mode[i] = -1;
		for (size_t p = 0; p < num_pan_parameters; ++p) {
			_current_value[i][p] = -1111; /* some invalid data that forces an update */
		}
	}

#ifdef __APPLE__
	AudioComponentDescription auDescription = {
		kAudioUnitType_Mixer,
		'3dem' /* kAudioUnitSubType_SpatialMixer */,
		kAudioUnitManufacturer_Apple,
		0,
		0
	};

	AudioComponent comp = AudioComponentFindNext (NULL, &auDescription);
	if (comp && noErr == AudioComponentInstanceNew (comp, &_au)) {
		ComponentResult err;

		AudioStreamBasicDescription streamFormat;
		streamFormat.mChannelsPerFrame = 12;
		streamFormat.mSampleRate       = _session.sample_rate ();
		streamFormat.mFormatID         = kAudioFormatLinearPCM;
		streamFormat.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
		streamFormat.mBitsPerChannel   = 32;
		streamFormat.mFramesPerPacket  = 1;
		streamFormat.mBytesPerPacket   = 4;
		streamFormat.mBytesPerFrame    = 4;

		err = AudioUnitSetProperty (_au,
		                            kAudioUnitProperty_StreamFormat,
		                            kAudioUnitScope_Input,
		                            0,
		                            &streamFormat,
		                            sizeof (AudioStreamBasicDescription));

		if (err != noErr) {
			return;
		}

		streamFormat.mChannelsPerFrame = 2;

		err = AudioUnitSetProperty (_au,
		                            kAudioUnitProperty_StreamFormat,
		                            kAudioUnitScope_Output,
		                            0,
		                            &streamFormat,
		                            sizeof (AudioStreamBasicDescription));

		if (err != noErr) {
			return;
		}

		AudioChannelLayout chanelLayout;
		chanelLayout.mChannelLayoutTag          = 0xc0000c; // kAudioChannelLayoutTag_Atmos_7_1_4;
		chanelLayout.mChannelBitmap             = 0;
		chanelLayout.mNumberChannelDescriptions = 0;

		err = AudioUnitSetProperty (_au,
		                            kAudioUnitProperty_AudioChannelLayout,
		                            kAudioUnitScope_Input,
		                            0,
		                            &chanelLayout,
		                            sizeof (chanelLayout));

		if (err != noErr) {
			return;
		}

		UInt32 renderingAlgorithm = 7; // kSpatializationAlgorithm_UseOutputType;

		err = AudioUnitSetProperty (_au,
		                            3000 /*kAudioUnitProperty_SpatializationAlgorithm*/,
		                            kAudioUnitScope_Input,
		                            0,
		                            &renderingAlgorithm,
		                            sizeof (renderingAlgorithm));

		if (err != noErr) {
			return;
		}

		UInt32 sourceMode = 3; // kSpatialMixerSourceMode_AmbienceBed;

		err = AudioUnitSetProperty (_au,
		                            3005 /*kAudioUnitProperty_SpatialMixerSourceMode*/,
		                            kAudioUnitScope_Input,
		                            0,
		                            &sourceMode,
		                            sizeof (sourceMode));

		if (err != noErr) {
			return;
		}

		AURenderCallbackStruct renderCallbackInfo;
		renderCallbackInfo.inputProc       = _render_callback;
		renderCallbackInfo.inputProcRefCon = this;

		err  = AudioUnitSetProperty (_au,
		                             kAudioUnitProperty_SetRenderCallback,
		                             kAudioUnitScope_Input,
		                             0, (void*)&renderCallbackInfo,
		                             sizeof (renderCallbackInfo));

		if (err != noErr) {
			return;
		}

		_au_buffers = (AudioBufferList*)malloc (offsetof (AudioBufferList, mBuffers) + 2 * sizeof (::AudioBuffer));

		_au_buffers->mNumberBuffers = 2;

		err = AudioUnitInitialize (_au);
		if (err != noErr) {
			return;
		}

		_have_au_renderer = true;
	}
#endif
}

SurroundReturn::~SurroundReturn ()
{
#ifdef __APPLE__
	if (_au) {
		AudioOutputUnitStop (_au);
		AudioUnitUninitialize (_au);
		CloseComponent (_au);
	}
	free (_au_buffers);
#endif
}

int
SurroundReturn::set_block_size (pframes_t nframes)
{
	_surround_bufs.ensure_buffers (DataType::AUDIO, 128, nframes);
	_surround_processor->set_block_size (nframes);
	return 0;
}

samplecnt_t
SurroundReturn::signal_latency () const
{
	return _surround_processor->signal_latency ();
}

void
SurroundReturn::flush ()
{
	_flush.store (1);
}

void
SurroundReturn::run (BufferSet& bufs, samplepos_t start_sample, samplepos_t end_sample, double speed, pframes_t nframes, bool)
{
	if (!check_active ()) {
		return;
	}

	int canderef (1);
	if (_flush.compare_exchange_strong (canderef, 0)) {
		_surround_processor->flush ();
	}

	bufs.set_count (_configured_output);
	_surround_bufs.silence (nframes, 0);

	RouteList rl = *_session.get_routes (); // XXX this allocates memory
	rl.sort (Stripable::Sorter (true));

	size_t id = 10; // First 10 IDs are reseved for bed mixes

	for (auto const& r : rl) {
		std::shared_ptr<SurroundSend> ss;
		if (!r->active ()) {
			continue;
		}
		if (!(ss = r->surround_send ()) || !ss->active ()) {
			continue;
		}

		timepos_t start, end;

		for (uint32_t s = 0; s < ss->bufs ().count ().n_audio () && id < max_object_id; ++s, ++id) {
			std::shared_ptr<SurroundPannable> const& p (ss->pan_param (s, start, end));

			AutoState const as        = p->automation_state ();
			bool const      automated = (as & Play) || ((as & (Touch | Latch)) && !p->touching ());

			AudioBuffer&       dst_ab (_surround_bufs.get_audio (id));
			AudioBuffer const& src_ab (ss->bufs ().get_audio (s));
			if (id > 9) {
				/* object */
				dst_ab.read_from (src_ab, nframes);
				if (!automated || start_sample >= end_sample) {
					pan_t const v[num_pan_parameters] = {
						(pan_t)p->pan_pos_x->get_value (),
						(pan_t)p->pan_pos_y->get_value (),
						(pan_t)p->pan_pos_z->get_value (),
						(pan_t)p->pan_size->get_value (),
						(pan_t)p->pan_snap->get_value ()
					};
					maybe_send_metadata (id, 0, v);
				} else {
					/* Evaluate Automation
					 *
					 * Note, exclusive end: range = [start_sample, end_sample[
					 * nframes == end_sample - start_sample
					 * IOW: end_sample == next cycle's start_sample;
					 */
					if (nframes < 2) {
						evaluate (id, p, timepos_t (start_sample), 0);
					} else {
						timepos_t start (start_sample);
						timepos_t end (end_sample - 1);
						while (true) {
							Evoral::ControlEvent next_event (timepos_t (Temporal::AudioTime), 0.0f);
							if (!p->find_next_event (start, end, next_event)) {
								break;
							}
							samplecnt_t pos = std::min (timepos_t (start_sample).distance (next_event.when).samples (), (samplecnt_t)nframes - 1);
							evaluate (id, p, next_event.when, pos);
							start = next_event.when;
						}
						/* end */
						evaluate (id, p, end, nframes - 1);
					}
				}
				/* configure near/mid/far - not sample-accurate */
				int const brm = p->binaural_render_mode->get_value ();
				if (brm != _current_render_mode[id]) {
					_current_render_mode[id] = brm;
#if defined(LV2_EXTENDED) && defined(HAVE_LV2_1_10_0)
					URIMap::URIDs const& urids = URIMap::instance ().urids;
					forge_int_msg (urids.surr_Settings, urids.surr_Channel, id, urids.surr_BinauralRenderMode, brm);
#endif
				}

			} else {
				/* bed mix */
				dst_ab.merge_from (src_ab, nframes);
			}
		}

		if (id >= max_object_id) {
			break;
		}
	}

	if (_current_n_objects != id) {
		_current_n_objects = id;
#if defined(LV2_EXTENDED) && defined(HAVE_LV2_1_10_0)
		URIMap::URIDs const& urids = URIMap::instance ().urids;
		forge_int_msg (urids.surr_Settings, urids.surr_ChannelCount, _current_n_objects);
#endif
	}

	if (_have_au_renderer && _binaural_render_control->get_value () != 0 && _output_format_control->get_value () != 0) {
		_output_format_control->set_value (0.0, PBD::Controllable::NoGroup);
	}

	MainOutputFormat target_output_format = _output_format_control->get_value () == 0 ? OUTPUT_FORMAT_7_1_4 : OUTPUT_FORMAT_5_1;

	if (_have_au_renderer && _binaural_render_control->get_value () != 0) {
		target_output_format = OUTPUT_FORMAT_7_1_4;
	}

	if (_current_output_format != target_output_format) {
		_current_output_format = target_output_format;
#if defined(LV2_EXTENDED) && defined(HAVE_LV2_1_10_0)
		URIMap::URIDs const& urids = URIMap::instance ().urids;
		forge_int_msg (urids.surr_Settings, urids.surr_OutputFormat, target_output_format);
#endif
	}

	uint32_t meter_nframes = nframes;
	uint32_t meter_offset  = 0;

	if (_exporting && _export_start >= start_sample && _export_start < end_sample && start_sample != end_sample) {
		_lufs_meter.reset ();
		meter_offset = _export_start - start_sample;
		meter_nframes -= meter_offset;

#if defined(LV2_EXTENDED) && defined(HAVE_LV2_1_10_0)
		/* trigger export */
		//std::cout << "SURR START EXPORT " << start_sample << " <= " << _export_start << " < " << end_sample << "\n";
		URIMap::URIDs const& urids = URIMap::instance ().urids;
		forge_int_msg (urids.surr_ExportStart, urids.time_frame, meter_offset);

		/* Re-transmit pan pos - using export-start */
		size_t id = 10; // First 10 IDs are reseved for bed mixes
		for (auto const& r : rl) {
			std::shared_ptr<SurroundSend> ss;
			if (!r->active ()) {
				continue;
			}
			if (!(ss = r->surround_send ()) || !ss->active ()) {
				continue;
			}
			timepos_t unused_start, unused_end;
			for (uint32_t s = 0; s < ss->bufs ().count ().n_audio () && id < max_object_id; ++s, ++id) {
				std::shared_ptr<SurroundPannable> const& p (ss->pan_param (s, unused_start, unused_end));

				AutoState const as        = p->automation_state ();
				bool const      automated = (as & Play) || ((as & (Touch | Latch)) && !p->touching ());
				if (id > 9) {
					if (!automated) {
						pan_t const v[num_pan_parameters] = {
							(pan_t)p->pan_pos_x->get_value (),
							(pan_t)p->pan_pos_y->get_value (),
							(pan_t)p->pan_pos_z->get_value (),
							(pan_t)p->pan_size->get_value (),
							(pan_t)p->pan_snap->get_value ()
						};
						maybe_send_metadata (id, 0, v, true);
					} else {
						evaluate (id, p, timepos_t (_export_start), 0, true);
					}
				}
			}
			if (id >= max_object_id) {
				break;
			}
		}
#endif
	}

	if (_exporting && _export_end >= start_sample && _export_end < end_sample) {
		meter_nframes = _export_end - start_sample;
#if defined(LV2_EXTENDED) && defined(HAVE_LV2_1_10_0)
		//std::cout << "SURR STOP EXPORT " << start_sample << " <= " << _export_end << " < " << end_sample << "\n";
		URIMap::URIDs const& urids = URIMap::instance ().urids;
		forge_int_msg (urids.surr_ExportStop, urids.time_frame, _export_end - start_sample);
#endif
	}

	_trim->set_gain_automation_buffer (_session.trim_automation_buffer ());
	_trim->setup_gain_automation (start_sample, end_sample, nframes);
	_trim->run (_surround_bufs, start_sample, end_sample, speed, nframes, true);

	_surround_processor->connect_and_run (_surround_bufs, start_sample, end_sample, speed, _in_map, _out_map, nframes, 0);

	BufferSet::iterator i = _surround_bufs.begin (DataType::AUDIO);
	for (BufferSet::iterator o = bufs.begin (DataType::AUDIO); o != bufs.end (DataType::AUDIO); ++i, ++o) {
		o->read_from (*i, nframes);
	}

	if (_exporting) {
		_rolling = true;
	} else if (_rolling && start_sample == end_sample) {
		_rolling = false;
	} else if (!_rolling && start_sample != end_sample) {
		_rolling = true;
		_lufs_meter.reset ();
	}

	float const* data[5] = {
		_surround_bufs.get_audio (14).data (meter_offset),
		_surround_bufs.get_audio (15).data (meter_offset),
		_surround_bufs.get_audio (16).data (meter_offset),
		_surround_bufs.get_audio (18).data (meter_offset),
		_surround_bufs.get_audio (19).data (meter_offset)
	};

	_lufs_meter.run (data, meter_nframes);

#ifdef __APPLE__
	if (_au && _have_au_renderer && _binaural_render_control->get_value () != 0) {
		for (uint32_t i = 0; i < 12; ++i) {
			_au_data[i] = _surround_bufs.get_audio (i).data (0);
		}

		_au_buffers->mNumberBuffers = 2;
		for (uint32_t i = 0; i < 2; ++i) {
			_au_buffers->mBuffers[i].mNumberChannels = 1;
			_au_buffers->mBuffers[i].mDataByteSize   = nframes * sizeof (Sample);
			_au_buffers->mBuffers[i].mData           = _surround_bufs.get_audio (12 + i).data (0);
		}
		AudioUnitRenderActionFlags flags = 0;
		AudioTimeStamp             ts;
		ts.mSampleTime = _au_samples_processed;
		ts.mFlags      = kAudioTimeStampSampleTimeValid;

		OSErr err = AudioUnitRender (_au, &flags, &ts, /*bus*/ 0, nframes, _au_buffers);
		if (err == noErr) {
			_au_samples_processed += nframes;
			uint32_t limit = std::min<uint32_t> (_au_buffers->mNumberBuffers, 2);
			for (uint32_t i = 0; i < limit; ++i) {
				if (_au_buffers->mBuffers[i].mData == 0 || _au_buffers->mBuffers[i].mNumberChannels != 1) {
					continue;
				}
				Sample* expected_buffer_address = bufs.get_audio (12 + i).data (0);
				if (expected_buffer_address != _au_buffers->mBuffers[i].mData) {
					memcpy (expected_buffer_address, _au_buffers->mBuffers[i].mData, nframes * sizeof (Sample));
				}
			}
		}
	}
#endif
}

void
SurroundReturn::forge_int_msg (uint32_t obj_id, uint32_t key, int val, uint32_t key2, int val2)
{
	URIMap::URIDs const& urids = URIMap::instance ().urids;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_set_buffer (&_forge, _atom_buf, sizeof (_atom_buf));
	lv2_atom_forge_frame_time (&_forge, 0);
	LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_object (&_forge, &frame, 1, obj_id);
	lv2_atom_forge_key (&_forge, key);
	lv2_atom_forge_int (&_forge, val);
	if (key2 > 0) {
		lv2_atom_forge_key (&_forge, key2);
		lv2_atom_forge_int (&_forge, val2);
	}
	lv2_atom_forge_pop (&_forge, &frame);
	_surround_processor->write_from_ui (0, urids.atom_eventTransfer, lv2_atom_total_size (msg), (const uint8_t*)msg);
}

void
SurroundReturn::maybe_send_metadata (size_t id, pframes_t sample, pan_t const v[num_pan_parameters], bool force)
{
	bool changed = false;
	for (size_t i = 0; i < num_pan_parameters; ++i) {
		if (_current_value[id][i] != v[i]) {
			changed = true;
		}
		_current_value[id][i] = v[i];
	}
	if (!changed && !force) {
		return;
	}
	URIMap::URIDs const& urids = URIMap::instance ().urids;

#if defined(LV2_EXTENDED) && defined(HAVE_LV2_1_10_0)
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_set_buffer (&_forge, _atom_buf, sizeof (_atom_buf));
	lv2_atom_forge_frame_time (&_forge, 0);
	LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_object (&_forge, &frame, 1, urids.surr_MetaData);
	lv2_atom_forge_key (&_forge, urids.time_frame);
	lv2_atom_forge_int (&_forge, sample);
	lv2_atom_forge_key (&_forge, urids.surr_Channel);
	lv2_atom_forge_int (&_forge, id);
	lv2_atom_forge_key (&_forge, urids.surr_PosX);
	lv2_atom_forge_float (&_forge, v[0]);
	lv2_atom_forge_key (&_forge, urids.surr_PosY);
	lv2_atom_forge_float (&_forge, v[1]);
	lv2_atom_forge_key (&_forge, urids.surr_PosZ);
	lv2_atom_forge_float (&_forge, v[2]);
	lv2_atom_forge_key (&_forge, urids.surr_Size);
	lv2_atom_forge_float (&_forge, v[3]);
	lv2_atom_forge_key (&_forge, urids.surr_Snap);
	lv2_atom_forge_bool (&_forge, v[4] > 0 ? true : false);
	lv2_atom_forge_pop (&_forge, &frame);

	_surround_processor->write_from_ui (0, urids.atom_eventTransfer, lv2_atom_total_size (msg), (const uint8_t*)msg);
#endif
}

void
SurroundReturn::evaluate (size_t id, std::shared_ptr<SurroundPannable> const& p, timepos_t const& when, pframes_t sample, bool force)
{
	bool        ok[num_pan_parameters];
	pan_t const v[num_pan_parameters] = {
		(pan_t)p->pan_pos_x->list ()->rt_safe_eval (when, ok[0]),
		(pan_t)p->pan_pos_y->list ()->rt_safe_eval (when, ok[1]),
		(pan_t)p->pan_pos_z->list ()->rt_safe_eval (when, ok[2]),
		(pan_t)p->pan_size->list ()->rt_safe_eval (when, ok[3]),
		(pan_t)p->pan_snap->list ()->rt_safe_eval (when, ok[4])
	};
	if (ok[0] && ok[1] && ok[2] && ok[3] && ok[4]) {
		maybe_send_metadata (id, sample, v, force);
	}
}

bool
SurroundReturn::can_support_io_configuration (const ChanCount& in, ChanCount& out)
{
	out = ChanCount (DataType::AUDIO, 14); // 7.1.4 + binaural
	return in.n_total () == 0;
}

void
SurroundReturn::set_playback_offset (samplecnt_t cnt)
{
	Processor::set_playback_offset (cnt);
	std::shared_ptr<RouteList const> rl (_session.get_routes ());
	for (auto const& r : *rl) {
		std::shared_ptr<SurroundSend> ss = r->surround_send ();
		if (ss) {
			ss->set_delay_out (cnt);
		}
	}
}

void
SurroundReturn::setup_export (std::string const& fn, samplepos_t ss, samplepos_t es)
{
	if (0 == _surround_processor->setup_export (fn.c_str ())) {
		//std::cout << "SurroundReturn::setup export "<< ss << " to " << es << "\n";
		_exporting    = true;
		_export_start = ss - effective_latency ();
		_export_end   = es - effective_latency ();
	}
}

void
SurroundReturn::finalize_export ()
{
	//std::cout << "SurroundReturn::finalize_export\n";
	_surround_processor->finalize_export ();
	_exporting    = false;
	_export_start = _export_end = 0;
}

float
SurroundReturn::momentary () const
{
	return _lufs_meter.momentary ();
}

float
SurroundReturn::max_momentary () const
{
	return _lufs_meter.max_momentary ();
}

float
SurroundReturn::integrated_loudness () const
{
	return _lufs_meter.integrated_loudness ();
}

float
SurroundReturn::max_dbtp () const
{
	return _lufs_meter.dbtp ();
}

int
SurroundReturn::set_state (XMLNode const& node, int version)
{
	int target_output_format;
	if (node.get_property (X_("output-format"), target_output_format)) {
		if (target_output_format == OUTPUT_FORMAT_5_1 || target_output_format == OUTPUT_FORMAT_7_1_4) {
			_output_format_control->set_value (target_output_format == OUTPUT_FORMAT_7_1_4 ? 0.0 : 1.0, PBD::Controllable::NoGroup);
		}
	}
	return _trim->set_state (node, version);
}

XMLNode&
SurroundReturn::state () const
{
	XMLNode& node (_trim->state ());
	node.set_property ("name", "SurrReturn");
	node.set_property ("type", "surreturn");
	node.set_property ("output-format", (int)_current_output_format);
	return node;
}

#ifdef __APPLE__
OSStatus
SurroundReturn::_render_callback (void*                       userData,
                                  AudioUnitRenderActionFlags* ioActionFlags,
                                  const AudioTimeStamp*       inTimeStamp,
                                  UInt32                      inBusNumber,
                                  UInt32                      inNumberSamples,
                                  AudioBufferList*            ioData)
{
	if (userData) {
		return ((SurroundReturn*)userData)->render_callback (ioActionFlags, inTimeStamp, inBusNumber, inNumberSamples, ioData);
	}
	return paramErr;
}

OSStatus
SurroundReturn::render_callback (AudioUnitRenderActionFlags*,
                                 const AudioTimeStamp*,
                                 UInt32           bus,
                                 UInt32           inNumberSamples,
                                 AudioBufferList* ioData)
{
	uint32_t limit = std::min<uint32_t> (ioData->mNumberBuffers, 12);

	for (uint32_t i = 0; i < limit; ++i) {
		ioData->mBuffers[i].mNumberChannels = 1;
		ioData->mBuffers[i].mDataByteSize   = sizeof (Sample) * inNumberSamples;
		ioData->mBuffers[i].mData           = _au_data[i];
	}
	return noErr;
}
#endif
