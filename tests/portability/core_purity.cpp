// DATABREAKs Engine 029 - compile-time core purity check.
//
// Built with tests/portability/poison as the FIRST system include directory:
// every forbidden platform header there (<thread>, <mutex>, <fstream>,
// <iostream>, <chrono>, <filesystem>, <cstdio>, POSIX/Windows/Qt headers, ...)
// contains #error, so this file fails to compile if any core, instrument or
// platform-interface header includes one, directly or through another header.
// platform/host/ is deliberately NOT included: host implementations may use
// the platform.

#include "../../core/audio/audio_engine.h"
#include "../../core/audio/audio_pipeline.h"
#include "../../core/audio/drum_engine.h"
#include "../../core/audio/drum_event_queue.h"
#include "../../core/audio/drum_pad_bank.h"
#include "../../core/audio/effects/audio_effect.h"
#include "../../core/audio/effects/compressor.h"
#include "../../core/audio/effects/distortion.h"
#include "../../core/audio/effects/effect_chain.h"
#include "../../core/audio/effects/equalizer.h"
#include "../../core/audio/effects/reverb.h"
#include "../../core/audio/effects/soft_clipper.h"
#include "../../core/audio/effects/tape_effect.h"
#include "../../core/audio/layering_mixer.h"
#include "../../core/audio/mixer.h"
#include "../../core/audio/sample_player.h"
#include "../../core/audio/sample_slice_map.h"
#include "../../core/audio/sample_voice_manager.h"
#include "../../core/audio/spsc_ring.h"
#include "../../core/audio/synth_event.h"
#include "../../core/audio/tape_machine.h"
#include "../../core/audio/tape_processor.h"
#include "../../core/audio/tape_stream_transport.h"
#include "../../core/audio/tape_streaming_engine.h"
#include "../../core/audio/tape_transport.h"
#include "../../core/sequencer/arrangement.h"
#include "../../core/sequencer/exact_int128.h"
#include "../../core/sequencer/musical_clock.h"
#include "../../core/sequencer/musical_time.h"
#include "../../core/sequencer/pattern.h"
#include "../../core/sequencer/pattern_bank.h"
#include "../../core/sequencer/pattern_io.h"
#include "../../core/sequencer/quantize.h"
#include "../../core/sequencer/recorded_track.h"
#include "../../core/sequencer/recorder.h"
#include "../../core/sequencer/sequencer.h"
#include "../../core/target/resource_model.h"
#include "../../core/target/target_profile.h"
#include "../../instruments/synthesizers/fm_synth.h"
#include "../../instruments/synthesizers/mono_synth.h"
#include "../../instruments/synthesizers/poly_synth.h"
#include "../../instruments/synthesizers/synth_engine.h"
#include "../../instruments/synthesizers/synth_parameters.h"
#include "../../instruments/synthesizers/synth_voices.h"
#include "../../platform/audio_io.h"
#include "../../platform/input.h"
#include "../../platform/storage.h"

int main() { return 0; }
