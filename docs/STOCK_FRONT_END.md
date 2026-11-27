# Stock microphone front end: what the binaries contain

This note records what was found by reading the stock Amazon audio libraries
that ship on the device's system partition, so the inventory does not depend on
those binaries still being at hand. It is a map of the stock front end and a
status line for each piece in LibreEcho, not a specification.

## Provenance and method

- Binaries: `libasp.so` (the ASP/AFE pipeline), `libgenericaspclient.so` (the
  `LASP_CMD_*` control surface), and `libamz_nb_audio_flt.so` (FIR, limiter and
  dynamic-range compressor). All are 32-bit ARM Android shared objects.
- Disassembly: `llvm-objdump -d --triple=thumbv7 <lib>`; the default ARM decode
  is wrong for these files and `-M force-thumb` is rejected by LLVM.
- Symbol and string extraction: `llvm-nm -DC` and `strings -a`.
- The libraries are not in any LibreEcho repository. They were copied off the
  device's system partition and archived separately.
- No stock `asp.cfg` or `AFE.cfg` was in the dump. The config **keys** and
  module names are reliable evidence; most numeric defaults are not, because
  they live in those files or in the constructor code.

## Inventory

| Stock module or key | What it is | LibreEcho today |
| --- | --- | --- |
| `MicPreProcess`, `micsHpf_`, `refsHpf_`, `FullAWD HPF`, `FHT HPF`, `hpfCoeffs`, `hpfBypass` | Per-mic and per-reference high-pass state before the array processing | One high-pass on the mono output only; `feature/second-order-highpass` (PR #560) makes it second order. Per-lane filtering before beamforming is still open. |
| `FBF_V2`, `EVD_BeamFormer`, `BeamFormingCoefs`, `ConfSBBeamFormer` | Fixed superdirective beamformer with published coefficients | `src/adapter/mic_beamformer.c`: 12 fixed beams at 30 degrees on lanes 0, 1, 3, 4. |
| `ConfSNRBeamSelector`, `ConfSNRBeamMerger`, `AllBandSNRBeamMetric`, `AlignedBeamMerger`, `BestBeamSynthesis` | SNR-based beam selection and beam merging | Energy-over-floor beam selection with hysteresis; single selected beam, no merger. `feature/adaptive-noise-covariance` (PR #559) adds a measured noise covariance. |
| `MicOcclusionDetector`, `AFEDiagMicCompensationMaxGain`, `AFEDiagMicCompensationNumberofMics` | Watches each microphone and reports compensation gain | `feature/mic-occlusion-watch` (PR #561): lane watch, exclusion from the average and automatic single mixes. |
| `MicDecayDetector`, `ConfMicEnergy` (`m_MicEnergyInitValue`, `m_MicEnergySmoothingFactor`, `m_MicSilenceThreshold`, `m_noiseFloor`, `m_diffThreshold`) | Per-channel level decay and silence tracking | Covered in spirit by the lane watch; no separate module. |
| `subDuplexVad*` (`FlrMstatLength`, `FlrMstatSubFrms`, `FlrAttack`, `FlrDecay`, `FlrFastAttack`, `FlrCountLim`, `FlrMinDelta`, `InitialNoiseLevel`, `MinNoiseLevel`) | VAD with a minimum-statistics noise floor | `feature/vad-minimum-statistics` (PR #562): one-second floor window, second-quietest frame, 12 dB drop limit, configured lower bound. |
| `DNNVAD`, `EdgeflowModel` | Neural VAD with a shipped model | Energy VAD; the ONNX wake model supplies the acoustics that matter for wake. |
| `NoiseReductor`, `Recursive Magnitude Estimation NR`, `Error: NR Type not supported` | Single- and multi-channel noise reduction on the voice path | **Not implemented.** The largest remaining signal-path gap. |
| `AEC_V2`, `VestaAec`, `Serial AEC`, `EchoReferenceWithExternalSync`, `ResidualEchoSuppressor`, `ERLE` | Echo cancellation and residual echo suppression against a playback reference | `waked` runs a time-domain AEC (`src/adapter/voice_aec.c`) when playback is active; no residual echo suppressor. |
| `Dereverberator`, `CDR`, `overSubtractionFactor`, `forgettingFactor` | Dereverberation | **Not implemented.** |
| `AGC` (full-band and subband), `MultiBandCompressor`, `NDVC`, `GainRampUp`, `MicGainRampUp` | Gain control on the voice and playback paths | Static digital gain and the codec PGA; no AGC, compressor, or gain ramp. |
| `AWD`, `AARLocalization`, `awdAzHistAngle`, `awdAzHistLik`, `awdAzRad`, `awdAzVar`, `KalmanSmoother` | Acoustic wake-word direction and user localization | Instantaneous beam direction of arrival only (`/api/v1/microphones/beams`); no wake-relative histogram or lock. |
| `Num AWD Angles`, `Normalize AWD Histogram`, `Histogram Samples Before WW`, `Histogram Samples After WW`, `postWakewordListenTimeMs` (in `ConfFhtCalibration`) | Factory/self-test that localizes a stimulus before and after a wake word | Not implemented. |
| `FalseWWPrevention`, `m_clipThresHigh`, `m_varianceThres` | Rejects wake words that coincide with clipping or a transient | Wake acceptance uses the VAD score and the model score; no dedicated clip/variance guard. |
| `LowPowerModeSoundDetector`, `m_diffRMSSmoothThres`, `m_slowVarianceThres`, `High Pass Filter` | Low-power sound detector with fast/slow RMS and variance | Separate clap and smoke-alarm detectors exist; no general sound detector. |
| `LASP_CMD_GET_MIC_RMS_LEVEL`, `LASP_CMD_GET_AFE_DEBUG_INFO`, `LASP_CMD_GET_ASP_DEBUG_INFO`, `LASP_CMD_AFE_DATA_CAPTURE`, `LASP_CMD_GET_ASR_PATH_LATENCY` | Control-plane queries for levels, diagnostics, capture and latency | Lane levels and beam state via `micd`; no AFE diagnostic channel or ASR path latency query. |

## Suggested order for the remaining work

1. **Noise reduction on the voice path.** It is a named AFE stage and the one
   missing signal-path block between the beamformer and the recogniser.
2. **Per-lane high-pass before the beamformer.** The stock keeps `micsHpf_`
   alongside the raw mic buffers; the current mono filter leaves correlated
   low-frequency energy in the covariance estimate and the beam weights.
3. **Wake-direction stabilization.** The stock localizes the wake talker and
   keeps the beam there; `le_beamformer_lock` exists but nothing in the daemon
   calls it.

## Evidence limits

These findings come from the stock binaries on a host: symbols, strings and
disassembly, with the LibreEcho behaviours checked by host unit tests. They are
not measurements from a device. Nothing here has been validated against a stock
`asp.cfg`, and no numeric default should be assumed from the key names alone.