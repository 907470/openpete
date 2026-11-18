// SPDX-FileCopyrightText: 2026 Tyson Cleary
// SPDX-License-Identifier: MIT

// SPU additions for the openpete recorders, compiled inside spu.cpp's SPU
// namespace after its state so they read the voices directly. spu.cpp
// includes openpete_hooks.h, which declares GetVoiceHead, at its top.

static_assert(NUM_VOICES == VOICE_COUNT, "SPU::VOICE_COUNT must equal the SPU's voice count");

/// Final-mix WAV dump for openpete's audio oracle: post per-voice volume, post reverb, post main volume, the exact
/// stereo stream the hardware emits. Armed only when OPENPETE_SPU_DUMP names a file.
static std::unique_ptr<WAVWriter> s_openpete_output_dump;

/// Opens the final-mix dump when OPENPETE_SPU_DUMP is set.
static void OpenpeteOutputDumpOpen()
{
  const char* dump_path = std::getenv("OPENPETE_SPU_DUMP");
  if (!dump_path)
    return;
  s_openpete_output_dump = std::make_unique<WAVWriter>();
  if (!s_openpete_output_dump->Open(dump_path, SAMPLE_RATE, 2))
  {
    ERROR_LOG("Failed to open SPU output dump '{}'", dump_path);
    s_openpete_output_dump.reset();
  }
  else
  {
    INFO_LOG("SPU output dump -> {}", dump_path);
  }
}

/// Appends one stereo frame (left, right) to the final-mix dump when it is open.
static void OpenpeteOutputDumpFrame(const s16* frame)
{
  if (s_openpete_output_dump)
    s_openpete_output_dump->WriteFrames(frame, 1);
}

/// Closes the final-mix dump; closing rewrites the RIFF and data chunk sizes from the frames written.
static void OpenpeteOutputDumpClose()
{
  if (!s_openpete_output_dump)
    return;
  Error error;
  if (!s_openpete_output_dump->Close(&error))
    ERROR_LOG("SPU output dump close failed: {}", error.GetDescription());
  s_openpete_output_dump.reset();
}

bool GetVoiceHead(u32 voice, VoiceHead* out)
{
  if (voice >= NUM_VOICES)
    return false;
  const Voice& v = s_state.voices[voice];
  out->voice = voice;
  out->start_address = ZeroExtend32(v.regs.adpcm_start_address) * 8u;
  out->current_address = ZeroExtend32(v.current_address) * 8u;
  out->counter = v.counter.bits;
  out->on = v.IsOn();
  return true;
}
