CC ?= cc
CROSS_COMPILE ?=
PREFIX ?= /usr/local
DESTDIR ?=
CSTD ?= -std=c99
WARN = -Wall -Wextra -Wpedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc/adapter
CFLAGS ?= -O2
BUILD = build
TARGET = $(BUILD)/libreecho-web
LOGD_TARGET = $(BUILD)/libreecho-logd
ADAPTER_TARGETS = $(BUILD)/libreecho-networkd $(BUILD)/libreecho-timed $(BUILD)/libreecho-audiod $(BUILD)/libreecho-micd $(BUILD)/libreecho-ledd $(BUILD)/libreecho-btd $(BUILD)/libreecho-airplayd $(BUILD)/libreecho-ttsd
NETWORKD_SOURCES = src/adapter/networkd.c src/adapter/adapter_server.c src/log.c
TIMED_SOURCES = src/adapter/timed.c src/log.c
AUDIOD_SOURCES = src/adapter/audiod.c src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c
MICD_SOURCES = src/adapter/micd.c src/adapter/adapter_server.c src/log.c
LEDD_SOURCES = src/adapter/ledd.c src/adapter/adapter_server.c src/log.c
BTD_SOURCES = src/adapter/btd.c src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c
AIRPLAYD_SOURCES = src/adapter/airplayd.c src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c
TTSD_SOURCES = src/adapter/ttsd.c src/adapter/tts_engine_mock.c src/adapter/adapter_server.c src/log.c
TTSD_SHERPA_CXX_SOURCES = src/adapter/tts_engine_sherpa.cpp
LOGD_SOURCES = src/logd.c src/log.c
SOURCES = src/main.c src/http_server.c src/api.c src/auth.c src/backend.c src/backend_mock.c src/backend_linux.c src/config_store.c src/event_bus.c src/json.c src/log.c src/adapter/adapter_client.c src/adapter/adapter_server.c
OBJECTS = $(SOURCES:src/%.c=$(BUILD)/%.o)
NETWORKD_OBJECTS = $(NETWORKD_SOURCES:src/%.c=$(BUILD)/%.o)
TIMED_OBJECTS = $(TIMED_SOURCES:src/%.c=$(BUILD)/%.o)
AUDIOD_OBJECTS = $(AUDIOD_SOURCES:src/%.c=$(BUILD)/%.o)
MICD_OBJECTS = $(MICD_SOURCES:src/%.c=$(BUILD)/%.o)
LEDD_OBJECTS = $(LEDD_SOURCES:src/%.c=$(BUILD)/%.o)
BTD_OBJECTS = $(BTD_SOURCES:src/%.c=$(BUILD)/%.o)
AIRPLAYD_OBJECTS = $(AIRPLAYD_SOURCES:src/%.c=$(BUILD)/%.o)
TTSD_OBJECTS = $(TTSD_SOURCES:src/%.c=$(BUILD)/%.o)
LOGD_OBJECTS = $(LOGD_SOURCES:src/%.c=$(BUILD)/%.o)
comma := ,
GC_LDFLAGS ?= $(if $(filter Darwin,$(shell uname -s)),-Wl$(comma)-dead_strip,-Wl$(comma)--gc-sections)

.PHONY: all adapters clean release test install deploy
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

# sherpa-onnx backed ttsd (cross-compiled ARM32, static).  Uses the real
# ZipVoice neural TTS engine instead of the mock sine chirp.  Requires
# SHERPA_PREFIX (sherpa-onnx install) and ORT_BUILD (onnxruntime build dir).
SHERPA_PREFIX ?= $(HOME)/workspace/sherpa-onnx-arm32/install
ORT_BUILD ?= $(HOME)/workspace/onnxruntime-src/build-arm32
ESPEAK_SRC ?= $(HOME)/workspace/sherpa-onnx-src/build-arm32/_deps/espeak_ng-src
FLITE_SRC ?= $(HOME)/workspace/flite-2.2
SHERPA_CXXFLAGS = -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c++17 -O2 \
	-Isrc -Isrc/adapter -I$(SHERPA_PREFIX)/include -I$(ESPEAK_SRC)/src/include \
	-I$(FLITE_SRC)/include
SHERPA_LDFLAGS = -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
	-static -static-libstdc++ -static-libgcc \
	-L$(SHERPA_PREFIX)/lib

$(BUILD)/tts_engine_sherpa.arm.o: $(TTSD_SHERPA_CXX_SOURCES)
	$(CROSS_COMPILE)g++ $(SHERPA_CXXFLAGS) -c $< -o $@

$(BUILD)/ttsd.arm.o: src/adapter/ttsd.c
	$(CROSS_COMPILE)$(CC) -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
		-D_POSIX_C_SOURCE=200809L -DLE_TTSD_ENGINE_SHERPA -std=c99 -O2 -Isrc -Isrc/adapter -c $< -o $@

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

adapters: $(ADAPTER_TARGETS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD) $(BUILD)/adapter
	$(CROSS_COMPILE)$(CC) $(CPPFLAGS) $(CSTD) $(WARN) $(CFLAGS) -Isrc -c $< -o $@

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
	install -m 0755 init/libreecho-web.init init/libreecho-logd.init init/libreecho-networkd.init init/libreecho-timed.init init/libreecho-audiod.init init/libreecho-micd.init init/libreecho-ledd.init init/libreecho-btd.init init/libreecho-airplayd.init init/libreecho-ttsd.init $(DESTDIR)/etc/init.d/
	install -m 0644 config/ntp.conf $(DESTDIR)/etc/libreecho/ntp.conf

deploy: all
	./deploy/push-adb.sh $(DEPLOY_ARGS)

clean:
	rm -f $(OBJECTS) $(NETWORKD_OBJECTS) $(TIMED_OBJECTS) $(AUDIOD_OBJECTS) $(MICD_OBJECTS) $(LEDD_OBJECTS) \
		$(LOGD_OBJECTS) $(BTD_OBJECTS) $(AIRPLAYD_OBJECTS) $(TTSD_OBJECTS) $(ADAPTER_TARGETS) $(TARGET) $(LOGD_TARGET)
