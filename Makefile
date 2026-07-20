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
ADAPTER_TARGETS = $(BUILD)/libreecho-networkd $(BUILD)/libreecho-audiod $(BUILD)/libreecho-ledd
NETWORKD_SOURCES = src/adapter/networkd.c src/adapter/adapter_server.c
AUDIOD_SOURCES = src/adapter/audiod.c src/adapter/adapter_server.c
LEDD_SOURCES = src/adapter/ledd.c src/adapter/adapter_server.c
SOURCES = src/main.c src/http_server.c src/api.c src/backend.c src/backend_mock.c src/backend_linux.c src/config_store.c src/event_bus.c src/json.c src/adapter/adapter_client.c src/adapter/adapter_server.c
OBJECTS = $(SOURCES:src/%.c=$(BUILD)/%.o)
NETWORKD_OBJECTS = $(NETWORKD_SOURCES:src/%.c=$(BUILD)/%.o)
AUDIOD_OBJECTS = $(AUDIOD_SOURCES:src/%.c=$(BUILD)/%.o)
LEDD_OBJECTS = $(LEDD_SOURCES:src/%.c=$(BUILD)/%.o)
comma := ,
GC_LDFLAGS ?= $(if $(filter Darwin,$(shell uname -s)),-Wl$(comma)-dead_strip,-Wl$(comma)--gc-sections)

.PHONY: all adapters clean release test install deploy
all: CPPFLAGS += -DLE_DEV_CONTROLS=1
all: $(TARGET) adapters

$(TARGET): $(OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-networkd: $(NETWORKD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(NETWORKD_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-audiod: $(AUDIOD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(AUDIOD_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/libreecho-ledd: $(LEDD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(LEDD_OBJECTS) $(LDFLAGS) -o $@

adapters: $(ADAPTER_TARGETS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD) $(BUILD)/adapter
	$(CROSS_COMPILE)$(CC) $(CPPFLAGS) $(CSTD) $(WARN) $(CFLAGS) -Isrc -c $< -o $@

release: clean
	$(MAKE) CROSS_COMPILE="$(CROSS_COMPILE)" CC="$(CC)" CFLAGS="-Os -ffunction-sections -fdata-sections" LDFLAGS="$(GC_LDFLAGS)" $(TARGET) $(ADAPTER_TARGETS)

test:
	$(MAKE) clean
	$(MAKE) all
	sh tests/run_tests.sh

install: $(TARGET) adapters
	install -d $(DESTDIR)$(PREFIX)/sbin $(DESTDIR)$(PREFIX)/share/libreecho/web $(DESTDIR)/etc/libreecho $(DESTDIR)/etc/init.d
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/sbin/libreecho-web
	install -m 0755 $(ADAPTER_TARGETS) $(DESTDIR)$(PREFIX)/sbin/
	cp -R web/* $(DESTDIR)$(PREFIX)/share/libreecho/web/
	install -m 0600 config/defaults.json $(DESTDIR)/etc/libreecho/web-config.json
	install -m 0755 init/libreecho-web.init init/libreecho-networkd.init init/libreecho-audiod.init init/libreecho-ledd.init $(DESTDIR)/etc/init.d/

deploy: all
	./deploy/push-adb.sh $(DEPLOY_ARGS)

clean:
	rm -f $(OBJECTS) $(ADAPTER_TARGETS) $(TARGET)
