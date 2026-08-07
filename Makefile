CC ?= cc
CROSS_COMPILE ?=
PREFIX ?= /usr/local
DESTDIR ?=
CSTD ?= -std=c99
WARN = -Wall -Wextra -Wpedantic
OS_VERSION ?= $(shell tr -d '\r\n' < VERSION)
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc/adapter -DLE_OS_VERSION=\"$(OS_VERSION)\"
CFLAGS ?= -O2
BUILD = build
TARGET = $(BUILD)/libreecho-web
LOGD_TARGET = $(BUILD)/libreecho-logd
ADAPTER_TARGETS = $(BUILD)/libreecho-networkd $(BUILD)/libreecho-timed $(BUILD)/libreecho-audiod $(BUILD)/libreecho-micd $(BUILD)/libreecho-ledd $(BUILD)/libreecho-btd $(BUILD)/libreecho-airplayd $(BUILD)/libreecho-ttsd $(BUILD)/libreecho-sttd $(BUILD)/libreecho-agentd $(BUILD)/libreecho-wyomingd $(BUILD)/libreecho-sttd-wyoming $(BUILD)/libreecho-ttsd-wyoming
NETWORKD_SOURCES = src/adapter/networkd.c src/adapter/adapter_server.c src/log.c
TIMED_SOURCES = src/adapter/timed.c src/log.c
AUDIOD_SOURCES = src/adapter/audiod.c src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c
MICD_SOURCES = src/adapter/micd.c src/adapter/voice_dsp.c src/adapter/adapter_server.c src/log.c
LEDD_SOURCES = src/adapter/ledd.c src/adapter/adapter_server.c src/log.c
BTD_SOURCES = src/adapter/btd.c src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c
AIRPLAYD_SOURCES = src/adapter/airplayd.c src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c
TTSD_SOURCES = src/adapter/ttsd.c src/adapter/tts_engine_mock.c src/adapter/adapter_server.c src/log.c
TTSD_SHERPA_CXX_SOURCES = src/adapter/tts_engine_sherpa.cpp
STTD_SOURCES = src/adapter/sttd.c src/adapter/stt_engine_mock.c src/adapter/adapter_server.c src/log.c
STTD_SHERPA_CXX_SOURCES = src/adapter/stt_engine_sherpa.cpp
AGENTD_SOURCES = src/adapter/agentd.c src/adapter/llm_provider.c \
	src/adapter/llm_codex.c src/adapter/llm_openai.c src/adapter/llm_http.c src/adapter/llm_store.c \
	src/adapter/voice_reply.c src/adapter/voice_playback.c \
	src/adapter/voice_pipeline.c src/adapter/voice_stream.c \
	src/adapter/voice_listening_led.c \
	src/adapter/adapter_client.c src/adapter/adapter_server.c \
	src/config_store.c src/json.c src/log.c
WYOMINGD_SOURCES = src/adapter/wyomingd.c src/adapter/wyoming_protocol.c \
	src/adapter/voice_stream.c src/adapter/voice_listening_led.c \
	src/adapter/adapter_client.c src/json.c src/log.c
LOGD_SOURCES = src/logd.c src/log.c
SOURCES = src/main.c src/http_server.c src/api.c src/auth.c src/backend.c src/backend_mock.c src/backend_linux.c src/config_store.c src/event_bus.c src/json.c src/log.c src/adapter/adapter_client.c src/adapter/adapter_server.c src/adapter/wyoming_client.c src/adapter/voice_stream.c
OBJECTS = $(SOURCES:src/%.c=$(BUILD)/%.o)
NETWORKD_OBJECTS = $(NETWORKD_SOURCES:src/%.c=$(BUILD)/%.o)
TIMED_OBJECTS = $(TIMED_SOURCES:src/%.c=$(BUILD)/%.o)
AUDIOD_OBJECTS = $(AUDIOD_SOURCES:src/%.c=$(BUILD)/%.o)
MICD_OBJECTS = $(MICD_SOURCES:src/%.c=$(BUILD)/%.o)
LEDD_OBJECTS = $(LEDD_SOURCES:src/%.c=$(BUILD)/%.o)
BTD_OBJECTS = $(BTD_SOURCES:src/%.c=$(BUILD)/%.o)
AIRPLAYD_OBJECTS = $(AIRPLAYD_SOURCES:src/%.c=$(BUILD)/%.o)
TTSD_OBJECTS = $(TTSD_SOURCES:src/%.c=$(BUILD)/%.o)
STTD_OBJECTS = $(STTD_SOURCES:src/%.c=$(BUILD)/%.o)
AGENTD_OBJECTS = $(AGENTD_SOURCES:src/%.c=$(BUILD)/%.o)
WYOMINGD_OBJECTS = $(WYOMINGD_SOURCES:src/%.c=$(BUILD)/%.o)
LOGD_OBJECTS = $(LOGD_SOURCES:src/%.c=$(BUILD)/%.o)
comma := ,
GC_LDFLAGS ?= $(if $(filter Darwin,$(shell uname -s)),-Wl$(comma)-dead_strip,-Wl$(comma)--gc-sections)

.PHONY: all adapters clean release test install deploy test-wyoming-protocol test-wyomingd
all: CPPFLAGS += -DLE_DEV_CONTROLS=1
all: $(TARGET) $(LOGD_TARGET) adapters

$(TARGET): $(OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(OBJECTS) $(LDFLAGS) -o $@

$(LOGD_TARGET): $(LOGD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(LOGD_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-networkd: $(NETWORKD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(NETWORKD_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-timed: $(TIMED_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(TIMED_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-audiod: $(AUDIOD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(AUDIOD_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-micd: $(MICD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(MICD_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-ledd: $(LEDD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(LEDD_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-btd: $(BTD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(BTD_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-airplayd: $(AIRPLAYD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(AIRPLAYD_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-ttsd: $(TTSD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(TTSD_OBJECTS) $(LDFLAGS) -lpthread -lm -o $@

$(BUILD)/libreecho-sttd: $(STTD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(STTD_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-agentd: $(AGENTD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(AGENTD_OBJECTS) $(LDFLAGS) \
		-lpthread -o $@

$(BUILD)/libreecho-wyomingd: $(WYOMINGD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(WYOMINGD_OBJECTS) $(LDFLAGS) -lm -o $@

$(BUILD)/libreecho-sttd-wyoming: src/adapter/sttd.c \
		src/adapter/stt_engine_wyoming.c src/adapter/wyoming_client.c \
		src/adapter/wyoming_protocol.c src/adapter/adapter_server.c \
		src/adapter/voice_dsp.c src/json.c src/log.c
	$(CROSS_COMPILE)$(CC) $(CPPFLAGS) $(CSTD) $(WARN) $(CFLAGS) -Isrc \
		$^ $(LDFLAGS) -lm -o $@

$(BUILD)/libreecho-ttsd-wyoming: src/adapter/ttsd.c \
		src/adapter/tts_engine_wyoming.c src/adapter/wyoming_client.c \
		src/adapter/wyoming_protocol.c src/adapter/adapter_server.c \
		src/json.c src/log.c
	$(CROSS_COMPILE)$(CC) $(CPPFLAGS) $(CSTD) $(WARN) $(CFLAGS) -Isrc \
		-DLE_TTSD_ENGINE_WYOMING $^ $(LDFLAGS) -lpthread -lm -o $@

$(BUILD)/test-wyoming-protocol: tests/test_wyoming_protocol.c \
		src/adapter/wyoming_protocol.c src/json.c
	$(CC) $(CSTD) $(WARN) -Werror -Isrc $^ -o $@

test-wyoming-protocol: $(BUILD)/test-wyoming-protocol
	./$(BUILD)/test-wyoming-protocol

$(BUILD)/test-wyomingd: tests/test_wyomingd.c $(BUILD)/libreecho-wyomingd
	$(CC) $(CSTD) $(WARN) -Werror -Isrc tests/test_wyomingd.c \
		src/adapter/voice_stream.c src/adapter/wyoming_protocol.c src/json.c \
		-o $@

test-wyomingd: $(BUILD)/test-wyomingd
	./$(BUILD)/test-wyomingd

# sherpa-onnx backed ttsd (cross-compiled ARM32, static).  Uses the real
# ZipVoice neural TTS engine instead of the mock sine chirp.  Requires
# SHERPA_PREFIX (sherpa-onnx install) and ORT_BUILD (onnxruntime build dir).
SHERPA_PREFIX ?= $(HOME)/workspace/sherpa-onnx-arm32/install
ORT_BUILD ?= $(HOME)/workspace/onnxruntime-src/build-arm32
ESPEAK_SRC ?= $(HOME)/workspace/sherpa-onnx-src/build-arm32/_deps/espeak_ng-src
FLITE_SRC ?= $(HOME)/workspace/flite-2.2
SPEEX_PREFIX ?= $(HOME)/workspace/speexdsp-host
ARM_SPEEX_PREFIX ?= $(HOME)/workspace/speexdsp-arm32
WAKE_ORT_BUILD ?= $(ORT_BUILD)
WAKE_ORT_SOURCE ?= $(HOME)/workspace/onnxruntime-src
WAKE_ARM_CXXFLAGS = -march=armv7-a -mfpu=neon-vfpv4 \
	-mfloat-abi=hard -std=c++17 -O3 -ffunction-sections \
	-fdata-sections -Wall -Wextra -Wpedantic -Werror \
	-Isrc -Isrc/adapter -I$(WAKE_ORT_SOURCE)/include
WAKE_ORT_ARCHIVES = \
	$(WAKE_ORT_BUILD)/libonnxruntime_session.a \
	$(WAKE_ORT_BUILD)/libonnxruntime_optimizer.a \
	$(WAKE_ORT_BUILD)/libonnxruntime_providers.a \
	$(WAKE_ORT_BUILD)/libonnxruntime_graph.a \
	$(WAKE_ORT_BUILD)/libonnxruntime_framework.a \
	$(WAKE_ORT_BUILD)/libonnxruntime_common.a \
	$(WAKE_ORT_BUILD)/libonnxruntime_mlas.a \
	$(WAKE_ORT_BUILD)/libonnxruntime_util.a \
	$(WAKE_ORT_BUILD)/libonnxruntime_flatbuffers.a \
	$(WAKE_ORT_BUILD)/libonnxruntime_lora.a \
	$(WAKE_ORT_BUILD)/_deps/onnx-build/libonnx.a \
	$(WAKE_ORT_BUILD)/_deps/onnx-build/libonnx_proto.a \
	$(WAKE_ORT_BUILD)/_deps/protobuf-build/libprotobuf-lite.a \
	$(WAKE_ORT_BUILD)/_deps/flatbuffers-build/libflatbuffers.a \
	$(HOME)/workspace/onnxruntime-arm32/install/lib/libre2.a
WAKE_ORT_ABSEIL = $$(find $(WAKE_ORT_BUILD)/_deps/abseil_cpp-build \
	-name '*.a')
SHERPA_CXXFLAGS = -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c++17 -O2 \
	-Isrc -Isrc/adapter -I$(SHERPA_PREFIX)/include -I$(ESPEAK_SRC)/src/include \
	-I$(FLITE_SRC)/include
SHERPA_LDFLAGS = -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
	-static -static-libstdc++ -static-libgcc \
	-L$(SHERPA_PREFIX)/lib

$(BUILD)/tts_engine_sherpa.arm.o: $(TTSD_SHERPA_CXX_SOURCES)
	$(CROSS_COMPILE)g++ $(SHERPA_CXXFLAGS) -c $< -o $@

$(BUILD)/stt_engine_sherpa.arm.o: $(STTD_SHERPA_CXX_SOURCES)
	$(CROSS_COMPILE)g++ $(SHERPA_CXXFLAGS) -c $< -o $@

$(BUILD)/ttsd.arm.o: src/adapter/ttsd.c
	$(CROSS_COMPILE)$(CC) -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
		-D_POSIX_C_SOURCE=200809L -DLE_TTSD_ENGINE_SHERPA -std=c99 -O2 -Isrc -Isrc/adapter -c $< -o $@

$(BUILD)/sttd.arm.o: src/adapter/sttd.c
	$(CROSS_COMPILE)$(CC) -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
		-D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Isrc -Isrc/adapter -c $< -o $@

$(BUILD)/adapter_server.arm.o: src/adapter/adapter_server.c
	$(CROSS_COMPILE)$(CC) -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
		-D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Isrc -Isrc/adapter -c $< -o $@

$(BUILD)/log.arm.o: src/log.c
	$(CROSS_COMPILE)$(CC) -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
		-D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Isrc -c $< -o $@

$(BUILD)/libreecho-ttsd-sherpa: $(BUILD)/ttsd.arm.o $(BUILD)/tts_engine_sherpa.arm.o \
		$(BUILD)/adapter_server.arm.o $(BUILD)/log.arm.o
	$(CROSS_COMPILE)g++ $(SHERPA_LDFLAGS) \
		$(BUILD)/ttsd.arm.o $(BUILD)/tts_engine_sherpa.arm.o \
		$(BUILD)/adapter_server.arm.o $(BUILD)/log.arm.o \
		-L$(FLITE_SRC)/build/arm-linux-gnueabihf/lib \
		-lflite_cmu_us_slt -lflite_cmu_us_awb -lflite_cmu_us_rms \
		-lflite_cmu_us_kal -lflite_usenglish -lflite_cmulex \
		-lflite_cmu_indic_lang -lflite_cmu_grapheme_lang \
		-lflite_cmu_indic_lex -lflite_cmu_grapheme_lex -lflite \
		-lsherpa-onnx-c-api -lsherpa-onnx-core -lsherpa-onnx-cxx-api \
		-lespeak-ng -lpiper_phonemize -lssentencepiece_core \
		-lkaldi-native-fbank-core -lkissfft-float \
		-lsherpa-onnx-fst -lsherpa-onnx-fstfar -lsherpa-onnx-kaldifst-core \
		-lkaldi-decoder-core -lucd \
		-Wl,--start-group \
		$(ORT_BUILD)/libonnxruntime_session.a \
		$(ORT_BUILD)/libonnxruntime_optimizer.a \
		$(ORT_BUILD)/libonnxruntime_providers.a \
		$(ORT_BUILD)/libonnxruntime_graph.a \
		$(ORT_BUILD)/libonnxruntime_framework.a \
		$(ORT_BUILD)/libonnxruntime_common.a \
		$(ORT_BUILD)/libonnxruntime_mlas.a \
		$(ORT_BUILD)/libonnxruntime_util.a \
		$(ORT_BUILD)/libonnxruntime_flatbuffers.a \
		$(ORT_BUILD)/libonnxruntime_lora.a \
		$(ORT_BUILD)/_deps/onnx-build/libonnx.a \
		$(ORT_BUILD)/_deps/onnx-build/libonnx_proto.a \
		$(ORT_BUILD)/_deps/protobuf-build/libprotobuf-lite.a \
		$(ORT_BUILD)/_deps/flatbuffers-build/libflatbuffers.a \
		$(HOME)/workspace/onnxruntime-arm32/install/lib/libre2.a \
		$$(find $(ORT_BUILD)/_deps/abseil_cpp-build -name '*.a') \
		-Wl,--end-group \
		-lpthread -ldl -lm -o $@

$(BUILD)/libreecho-sttd-sherpa-arm32: $(BUILD)/sttd.arm.o \
		$(BUILD)/stt_engine_sherpa.arm.o $(BUILD)/adapter_server.arm.o
	$(CROSS_COMPILE)g++ $(SHERPA_LDFLAGS) \
		$(BUILD)/sttd.arm.o $(BUILD)/stt_engine_sherpa.arm.o \
		$(BUILD)/adapter_server.arm.o \
		-lsherpa-onnx-c-api -lsherpa-onnx-core -lsherpa-onnx-cxx-api \
		-lkaldi-native-fbank-core -lkissfft-float \
		-lsherpa-onnx-fst -lsherpa-onnx-fstfar \
		-lsherpa-onnx-kaldifst-core -lkaldi-decoder-core \
		-lespeak-ng -lpiper_phonemize -lssentencepiece_core -lucd \
		-Wl,--start-group \
		$(ORT_BUILD)/libonnxruntime_session.a \
		$(ORT_BUILD)/libonnxruntime_optimizer.a \
		$(ORT_BUILD)/libonnxruntime_providers.a \
		$(ORT_BUILD)/libonnxruntime_graph.a \
		$(ORT_BUILD)/libonnxruntime_framework.a \
		$(ORT_BUILD)/libonnxruntime_common.a \
		$(ORT_BUILD)/libonnxruntime_mlas.a \
		$(ORT_BUILD)/libonnxruntime_util.a \
		$(ORT_BUILD)/libonnxruntime_flatbuffers.a \
		$(ORT_BUILD)/libonnxruntime_lora.a \
		$(ORT_BUILD)/_deps/onnx-build/libonnx.a \
		$(ORT_BUILD)/_deps/onnx-build/libonnx_proto.a \
		$(ORT_BUILD)/_deps/protobuf-build/libprotobuf-lite.a \
		$(ORT_BUILD)/_deps/flatbuffers-build/libflatbuffers.a \
		$(HOME)/workspace/onnxruntime-arm32/install/lib/libre2.a \
		$$(find $(ORT_BUILD)/_deps/abseil_cpp-build -name '*.a') \
		-Wl,--end-group -lpthread -ldl -lm -o $@

$(BUILD)/test-voice-aec: tests/test_voice_aec.c src/adapter/voice_aec.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc -I$(SPEEX_PREFIX)/include \
		$^ $(SPEEX_PREFIX)/lib/libspeexdsp.a -lm -o $@

$(BUILD)/test-voice-reference: tests/test_voice_reference.c src/adapter/voice_reference.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc -I$(SPEEX_PREFIX)/include \
		$^ $(SPEEX_PREFIX)/lib/libspeexdsp.a -lm -o $@

$(BUILD)/test-wake-led: tests/test_wake_led.c src/adapter/wake_led.c \
		src/adapter/adapter_client.c src/log.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc $^ -o $@

$(BUILD)/test-voice-stream: tests/test_voice_stream.c \
		src/adapter/voice_stream.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc $^ -o $@

$(BUILD)/test-sttd: tests/test_sttd.c src/adapter/adapter_client.c \
		src/log.c $(BUILD)/libreecho-sttd
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc tests/test_sttd.c \
		src/adapter/adapter_client.c src/log.c -o $@

$(BUILD)/test-llm-provider: tests/test_llm_provider.c \
		src/adapter/llm_provider.c src/adapter/llm_codex.c \
		src/adapter/llm_openai.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc $^ -o $@

$(BUILD)/mock-llm-curl: tests/mock_llm_curl.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror $< -o $@

$(BUILD)/mock-audio-adapter: tests/mock_audio_adapter.c \
		src/adapter/adapter_server.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc $^ -o $@

$(BUILD)/test-llm-http: tests/test_llm_http.c src/adapter/llm_http.c \
		$(BUILD)/mock-llm-curl
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc tests/test_llm_http.c \
		src/adapter/llm_http.c -o $@

$(BUILD)/test-llm-store: tests/test_llm_store.c src/adapter/llm_store.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc $^ -o $@

$(BUILD)/test-agentd: tests/test_agentd.c src/adapter/adapter_client.c \
		src/log.c $(BUILD)/libreecho-agentd $(BUILD)/mock-llm-curl \
		$(BUILD)/mock-audio-adapter $(BUILD)/libreecho-sttd \
		$(BUILD)/mock-voice-source
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc tests/test_agentd.c \
		src/adapter/adapter_client.c src/log.c -o $@

$(BUILD)/test-voice-reply: tests/test_voice_reply.c \
		src/adapter/voice_reply.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc $^ -o $@

$(BUILD)/test-voice-playback: tests/test_voice_playback.c \
		src/adapter/voice_playback.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc $^ -lpthread -o $@

$(BUILD)/mock-voice-source: tests/mock_voice_source.c \
		src/adapter/voice_stream.c src/adapter/adapter_server.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc $^ -o $@

$(BUILD)/test-voice-pipeline: tests/test_voice_pipeline.c \
		src/adapter/voice_pipeline.c src/adapter/voice_stream.c \
		src/adapter/voice_listening_led.c src/adapter/adapter_client.c \
		src/json.c $(BUILD)/libreecho-sttd $(BUILD)/mock-voice-source
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc tests/test_voice_pipeline.c \
		src/adapter/voice_pipeline.c src/adapter/voice_stream.c \
		src/adapter/voice_listening_led.c src/adapter/adapter_client.c \
		src/json.c src/log.c -lpthread -o $@

$(BUILD)/libreecho-waked: src/adapter/waked.c src/adapter/voice_aec.c \
		src/adapter/voice_reference.c src/adapter/voice_dsp.c \
		src/adapter/voice_stream.c \
		src/adapter/wake_led.c src/adapter/adapter_client.c \
		src/adapter/adapter_server.c src/log.c
	$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra \
		-Wpedantic -Werror -Isrc -I$(SPEEX_PREFIX)/include \
		$^ $(SPEEX_PREFIX)/lib/libspeexdsp.a -lm -o $@

$(BUILD)/libreecho-waked-arm32: src/adapter/waked.c src/adapter/voice_aec.c \
		src/adapter/voice_reference.c src/adapter/voice_dsp.c \
		src/adapter/voice_stream.c \
		src/adapter/wake_led.c src/adapter/adapter_client.c src/log.c
	$(CROSS_COMPILE)$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -O3 \
		-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
		-ffunction-sections -fdata-sections -Wall -Wextra \
		-Wpedantic -Werror -Isrc -I$(ARM_SPEEX_PREFIX)/include \
		$^ $(ARM_SPEEX_PREFIX)/lib/libspeexdsp.a \
		-static -Wl,--gc-sections -lm -o $@

$(BUILD)/wake_engine_onnx.arm.o: src/adapter/wake_engine_onnx.cpp
	@mkdir -p $(BUILD)
	$(CROSS_COMPILE)g++ $(WAKE_ARM_CXXFLAGS) -c $< -o $@

$(BUILD)/test_wake_engine.arm.o: tests/test_wake_engine.cpp
	@mkdir -p $(BUILD)
	$(CROSS_COMPILE)g++ $(WAKE_ARM_CXXFLAGS) -c $< -o $@

$(BUILD)/test-wake-engine-arm32: $(BUILD)/wake_engine_onnx.arm.o \
		$(BUILD)/test_wake_engine.arm.o
	$(CROSS_COMPILE)g++ -march=armv7-a -mfpu=neon-vfpv4 \
		-mfloat-abi=hard -static -static-libstdc++ -static-libgcc \
		-Wl,--gc-sections -o $@ $^ -Wl,--start-group \
		$(WAKE_ORT_ARCHIVES) $(WAKE_ORT_ABSEIL) -Wl,--end-group \
		-lpthread -ldl -lm

$(BUILD)/wake-adapter-client-arm32: tests/wake_adapter_client.c \
		src/adapter/adapter_client.c src/log.c
	$(CROSS_COMPILE)$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -Os \
		-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
		-ffunction-sections -fdata-sections -Wall -Wextra \
		-Wpedantic -Werror -Isrc $^ -static \
		-Wl,--gc-sections -o $@

$(BUILD)/stt-stream-file-client-arm32: tests/stt_stream_file_client.c
	$(CROSS_COMPILE)$(CC) -D_POSIX_C_SOURCE=200809L -std=c99 -Os \
		-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
		-ffunction-sections -fdata-sections -Wall -Wextra \
		-Wpedantic -Werror $< -static -Wl,--gc-sections -o $@

$(BUILD)/%.wake.arm.o: src/adapter/%.c
	@mkdir -p $(BUILD)
	$(CROSS_COMPILE)$(CC) -D_POSIX_C_SOURCE=200809L \
		-DLE_WAKE_ENGINE_ONNX -std=c99 -O3 -march=armv7-a \
		-mfpu=neon-vfpv4 -mfloat-abi=hard -ffunction-sections \
		-fdata-sections -Wall -Wextra -Wpedantic -Werror \
		-Isrc -Isrc/adapter -I$(ARM_SPEEX_PREFIX)/include \
		-c $< -o $@

$(BUILD)/waked.wake.arm.o: src/adapter/adapter.h \
	src/adapter/voice_aec.h src/adapter/voice_dsp.h \
	src/adapter/voice_reference.h src/adapter/voice_stream.h \
	src/adapter/wake_led.h \
	src/adapter/wake_worker.h
$(BUILD)/voice_aec.wake.arm.o: src/adapter/voice_aec.h
$(BUILD)/voice_reference.wake.arm.o: src/adapter/voice_reference.h
$(BUILD)/voice_dsp.wake.arm.o: src/adapter/voice_dsp.h
$(BUILD)/wake_worker.wake.arm.o: src/adapter/wake_worker.h \
	src/adapter/wake_engine.h
$(BUILD)/wake_engine_onnx.arm.o: src/adapter/wake_engine.h

$(BUILD)/log.wake.arm.o: src/log.c
	@mkdir -p $(BUILD)
	$(CROSS_COMPILE)$(CC) -D_POSIX_C_SOURCE=200809L \
		-DLE_WAKE_ENGINE_ONNX -std=c99 -O3 -march=armv7-a \
		-mfpu=neon-vfpv4 -mfloat-abi=hard -ffunction-sections \
		-fdata-sections -Wall -Wextra -Wpedantic -Werror \
		-Isrc -Isrc/adapter -c $< -o $@

WAKE_DAEMON_ARM_OBJECTS = $(BUILD)/waked.wake.arm.o \
	$(BUILD)/voice_aec.wake.arm.o \
	$(BUILD)/voice_reference.wake.arm.o \
	$(BUILD)/voice_dsp.wake.arm.o \
	$(BUILD)/voice_stream.wake.arm.o \
	$(BUILD)/wake_worker.wake.arm.o \
	$(BUILD)/wake_led.wake.arm.o \
	$(BUILD)/adapter_client.wake.arm.o \
	$(BUILD)/adapter_server.wake.arm.o \
	$(BUILD)/log.wake.arm.o \
	$(BUILD)/wake_engine_onnx.arm.o

$(BUILD)/libreecho-waked-onnx-arm32: $(WAKE_DAEMON_ARM_OBJECTS)
	$(CROSS_COMPILE)g++ -march=armv7-a -mfpu=neon-vfpv4 \
		-mfloat-abi=hard -static -static-libstdc++ -static-libgcc \
		-Wl,--gc-sections -o $@ $^ \
		$(ARM_SPEEX_PREFIX)/lib/libspeexdsp.a \
		-Wl,--start-group $(WAKE_ORT_ARCHIVES) \
		$(WAKE_ORT_ABSEIL) -Wl,--end-group -lpthread -ldl -lm

adapters: $(ADAPTER_TARGETS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD) $(BUILD)/adapter
	$(CROSS_COMPILE)$(CC) $(CPPFLAGS) $(CSTD) $(WARN) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/backend_linux.o $(BUILD)/backend_mock.o: VERSION src/version.h

release: clean
	$(MAKE) CROSS_COMPILE="$(CROSS_COMPILE)" CC="$(CC)" CFLAGS="-Os -ffunction-sections -fdata-sections" LDFLAGS="$(GC_LDFLAGS)" $(TARGET) $(LOGD_TARGET) $(ADAPTER_TARGETS)

test:
	$(MAKE) clean
	$(MAKE) all
	sh tests/run_tests.sh

install: $(TARGET) $(LOGD_TARGET) adapters
	install -d $(DESTDIR)$(PREFIX)/sbin $(DESTDIR)$(PREFIX)/share/libreecho/web $(DESTDIR)/etc/libreecho $(DESTDIR)/etc/init.d $(DESTDIR)/var/log/libreecho
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/sbin/libreecho-web
	install -m 0755 $(LOGD_TARGET) $(DESTDIR)$(PREFIX)/sbin/libreecho-logd
	install -m 0755 $(ADAPTER_TARGETS) $(DESTDIR)$(PREFIX)/sbin/
	cp -R web/* $(DESTDIR)$(PREFIX)/share/libreecho/web/
	install -m 0600 config/defaults.json $(DESTDIR)/etc/libreecho/web-config.json
	install -m 0755 init/libreecho-web.init init/libreecho-logd.init init/libreecho-networkd.init init/libreecho-timed.init init/libreecho-audiod.init init/libreecho-micd.init init/libreecho-ledd.init init/libreecho-btd.init init/libreecho-airplayd.init init/libreecho-ttsd.init init/libreecho-waked.init init/libreecho-sttd.init init/libreecho-agentd.init init/libreecho-wyomingd.init $(DESTDIR)/etc/init.d/
	install -m 0644 config/ntp.conf $(DESTDIR)/etc/libreecho/ntp.conf

deploy: all
	./deploy/push-adb.sh $(DEPLOY_ARGS)

clean:
	rm -f $(OBJECTS) $(NETWORKD_OBJECTS) $(TIMED_OBJECTS) $(AUDIOD_OBJECTS) $(MICD_OBJECTS) $(LEDD_OBJECTS) \
		$(LOGD_OBJECTS) $(BTD_OBJECTS) $(AIRPLAYD_OBJECTS) $(TTSD_OBJECTS) \
		$(STTD_OBJECTS) $(AGENTD_OBJECTS) $(WYOMINGD_OBJECTS) $(ADAPTER_TARGETS) \
		$(TARGET) $(LOGD_TARGET)
	rm -f $(BUILD)/libreecho-waked $(BUILD)/libreecho-waked-arm32 \
		$(BUILD)/libreecho-waked-onnx-arm32 \
		$(BUILD)/test-voice-aec $(BUILD)/test-voice-reference \
		$(BUILD)/test-wake-led $(BUILD)/test-voice-stream \
		$(BUILD)/test-sttd $(BUILD)/test-llm-provider \
		$(BUILD)/test-llm-http $(BUILD)/mock-llm-curl \
		$(BUILD)/test-wyoming-protocol $(BUILD)/test-wyomingd \
		$(BUILD)/mock-audio-adapter \
		$(BUILD)/test-llm-store \
		$(BUILD)/test-agentd \
		$(BUILD)/test-voice-reply \
		$(BUILD)/test-voice-playback \
		$(BUILD)/libreecho-sttd-sherpa-arm32 \
		$(BUILD)/sttd.arm.o $(BUILD)/stt_engine_sherpa.arm.o \
		$(BUILD)/test-wake-engine-arm32 $(BUILD)/wake-adapter-client-arm32 \
		$(BUILD)/stt-stream-file-client-arm32 \
		$(BUILD)/*.wake.arm.o $(BUILD)/wake_engine_onnx.arm.o \
		$(BUILD)/test_wake_engine.arm.o
