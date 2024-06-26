// Copyright 2019 Google LLC, Andrew Hines
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file replaces visqol_manager.cc in the normal visqol distribution,
// since it lets us build ViSQOL without redundant TensorFlow dependencies.

#include "visqol_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "absl/base/internal/raw_logging.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "alignment.h"
#include "analysis_window.h"
#include "audio_signal.h"
#include "gammatone_filterbank.h"
#include "misc_audio.h"
#include "neurogram_similiarity_index_measure.h"
#include "similarity_result.h"
#include "speech_similarity_to_quality_mapper.h"
#include "src/proto/similarity_result.pb.h"  // Generated by cc_proto_library rule
#include "status_macros.h"
#include "vad_patch_creator.h"
#include "visqol.h"

namespace Visqol {

const size_t k16kSampleRate = 16000;
const size_t k48kSampleRate = 48000;
const size_t VisqolManager::kPatchSize = 30;
const size_t VisqolManager::kPatchSizeSpeech = 20;
const size_t VisqolManager::kNumBandsAudio = 32;
const size_t VisqolManager::kNumBandsSpeech = 21;
const double VisqolManager::kMinimumFreq = 50;  // wideband
const double VisqolManager::kOverlap = 0.25;    // 25% overlap
const double VisqolManager::kDurationMismatchTolerance = 1.0;

absl::Status VisqolManager::Init(
    const FilePath& similarity_to_quality_mapper_model, bool use_speech_mode,
    bool use_unscaled_speech, int search_window, bool use_lattice_model) {
  use_speech_mode_ = use_speech_mode;
  assert(use_speech_mode == false);
  use_unscaled_speech_mos_mapping_ = use_unscaled_speech;
  search_window_ = search_window;
  use_lattice_model_ = use_lattice_model;

  InitPatchCreator();
  InitPatchSelector();
  InitSpectrogramBuilder();
  auto status =
      InitSimilarityToQualityMapper(similarity_to_quality_mapper_model);

  if (status.ok()) {
    is_initialized_ = true;
  } else {
    ABSL_RAW_LOG(ERROR, "%s", status.ToString().c_str());
  }

  return status;
}

absl::Status VisqolManager::Init(
    absl::string_view similarity_to_quality_mapper_model_string,
    bool use_speech_mode, bool use_unscaled_speech, int search_window,
    bool use_lattice_model) {
  return Init(FilePath(similarity_to_quality_mapper_model_string),
              use_speech_mode, use_unscaled_speech, search_window,
              use_lattice_model);
}

void VisqolManager::InitPatchCreator() {
  if (use_speech_mode_) {
    patch_creator_ = absl::make_unique<VadPatchCreator>(kPatchSizeSpeech);
  } else {
    patch_creator_ = absl::make_unique<ImagePatchCreator>(kPatchSize);
  }
}

void VisqolManager::InitPatchSelector() {
  // Setup the patch similarity comparator to use the Neurogram.
  patch_selector_ = absl::make_unique<ComparisonPatchesSelector>(
      absl::make_unique<NeurogramSimiliarityIndexMeasure>());
}

void VisqolManager::InitSpectrogramBuilder() {
  if (use_speech_mode_) {
    spectrogram_builder_ = absl::make_unique<GammatoneSpectrogramBuilder>(
        GammatoneFilterBank{kNumBandsSpeech, kMinimumFreq}, true);
  } else {
    spectrogram_builder_ = absl::make_unique<GammatoneSpectrogramBuilder>(
        GammatoneFilterBank{kNumBandsAudio, kMinimumFreq}, false);
  }
}

absl::Status VisqolManager::InitSimilarityToQualityMapper(
    FilePath sim_to_quality_mapper_model) {
  if (use_lattice_model_) {
    ABSL_RAW_LOG(
        WARNING,
        "Lattice models are not yet supported for audio mode, falling back "
        "to SVR model.");
  }
  sim_to_qual_ = absl::make_unique<SvrSimilarityToQualityMapper>(
      sim_to_quality_mapper_model);
  return sim_to_qual_->Init();
}

absl::StatusOr<SimilarityResultMsg> VisqolManager::Run(
    const FilePath& ref_signal_path, const FilePath& deg_signal_path) {
  // Ensure the initialization succeeded.
  VISQOL_RETURN_IF_ERROR(ErrorIfNotInitialized());

  // Load the wav audio files as mono.
  const AudioSignal ref_signal = MiscAudio::LoadAsMono(ref_signal_path);
  AudioSignal deg_signal = MiscAudio::LoadAsMono(deg_signal_path);

  // If the sim result was successfully calculated, set the signal file paths.
  // Else, return the StatusOr failure.
  SimilarityResultMsg sim_result_msg;
  VISQOL_ASSIGN_OR_RETURN(sim_result_msg, Run(ref_signal, deg_signal));
  sim_result_msg.set_reference_filepath(ref_signal_path.Path());
  sim_result_msg.set_degraded_filepath(deg_signal_path.Path());
  return sim_result_msg;
}

absl::StatusOr<SimilarityResultMsg> VisqolManager::Run(
    const AudioSignal& ref_signal, AudioSignal& deg_signal) {
  // Ensure the initialization succeeded.
  VISQOL_RETURN_IF_ERROR(ErrorIfNotInitialized());

  VISQOL_RETURN_IF_ERROR(ValidateInputAudio(ref_signal, deg_signal));

  // Adjust for codec initial padding.
  auto alignment_result = Alignment::GloballyAlign(ref_signal, deg_signal);
  deg_signal = std::get<0>(alignment_result);

  const AnalysisWindow window{ref_signal.sample_rate, kOverlap};

  // If the sim result is successfully calculated, populate the protobuf msg.
  // Else, return the StatusOr failure.
  const Visqol visqol;
  SimilarityResult sim_result;
  VISQOL_ASSIGN_OR_RETURN(
      sim_result, visqol.CalculateSimilarity(
                      ref_signal, deg_signal, spectrogram_builder_.get(),
                      window, patch_creator_.get(), patch_selector_.get(),
                      sim_to_qual_.get(), search_window_));
  SimilarityResultMsg sim_result_msg = PopulateSimResultMsg(sim_result);
  sim_result_msg.set_alignment_lag_s(std::get<1>(alignment_result));
  return sim_result_msg;
}

SimilarityResultMsg VisqolManager::PopulateSimResultMsg(
    const SimilarityResult& sim_result) {
  SimilarityResultMsg sim_result_msg;
  sim_result_msg.set_moslqo(sim_result.moslqo);
  sim_result_msg.set_vnsim(sim_result.vnsim);

  for (double val : sim_result.fvnsim) {
    sim_result_msg.add_fvnsim(val);
  }

  for (double val : sim_result.fvnsim10) {
    sim_result_msg.add_fvnsim10(val);
  }

  for (double val : sim_result.fstdnsim) {
    sim_result_msg.add_fstdnsim(val);
  }

  for (double val : sim_result.center_freq_bands) {
    sim_result_msg.add_center_freq_bands(val);
  }

  for (double val : sim_result.fvdegenergy) {
    sim_result_msg.add_fvdegenergy(val);
  }

  for (const PatchSimilarityResult& patch : sim_result.debug_info.patch_sims) {
    SimilarityResultMsg_PatchSimilarityMsg* patch_msg =
        sim_result_msg.add_patch_sims();
    patch_msg->set_similarity(patch.similarity);
    patch_msg->set_ref_patch_start_time(patch.ref_patch_start_time);
    patch_msg->set_ref_patch_end_time(patch.ref_patch_end_time);
    patch_msg->set_deg_patch_start_time(patch.deg_patch_start_time);
    patch_msg->set_deg_patch_end_time(patch.deg_patch_end_time);
    for (double each_fbm : patch.freq_band_means.ToVector()) {
      patch_msg->add_freq_band_means(each_fbm);
    }
  }

  return sim_result_msg;
}

absl::Status VisqolManager::ErrorIfNotInitialized() {
  if (is_initialized_ == false) {
    return absl::Status(absl::StatusCode::kAborted,
                        "VisqolManager must be initialized before use.");
  } else {
    return absl::Status();
  }
}

absl::Status VisqolManager::ValidateInputAudio(const AudioSignal& ref_signal,
                                               const AudioSignal& deg_signal) {
  // Warn if there is an excessive difference in durations.
  double ref_duration = ref_signal.GetDuration();
  double deg_duration = deg_signal.GetDuration();
  if (std::abs(ref_duration - deg_duration) > kDurationMismatchTolerance) {
    ABSL_RAW_LOG(WARNING,
                 "Mismatch in duration between reference and "
                 "degraded signal. Reference is %.2f seconds. Degraded is "
                 "%.2f seconds.",
                 ref_duration, deg_duration);
  }

  // Error if the signals have different sample rates.
  if (ref_signal.sample_rate != deg_signal.sample_rate) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Input audio signals have different sample rates! Reference audio "
        "sample rate: ",
        ref_signal.sample_rate,
        ". Degraded audio sample rate: ", deg_signal.sample_rate));
  }

  if (use_speech_mode_) {
    // Warn if input sample rate is > 16khz.
    if (ref_signal.sample_rate > k16kSampleRate) {
      ABSL_RAW_LOG(WARNING,
                   "Input audio sample rate is above 16kHz, which"
                   " may have undesired effects for speech mode.  Consider"
                   " resampling to 16kHz.");
    }
  } else {
    // Warn if the signals' sample rate is not 48k for full audio mode.
    if (ref_signal.sample_rate != k48kSampleRate) {
      ABSL_RAW_LOG(WARNING,
                   "Input audio does not have the expected sample"
                   " rate of 48kHz! This may negatively effect the prediction"
                   " of the MOS-LQO  score.");
    }
  }

  return absl::Status();
}
}  // namespace Visqol
