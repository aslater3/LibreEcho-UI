CC ?= cc
CROSS_COMPILE ?=
PREFIX ?= /usr/local
DESTDIR ?=
CSTD ?= -std=c99
WARN = -Wall -Wextra -Wpedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2
BUILD = build
TARGET = $(BUILD)/libreecho-web
comma := ,
GC_LDFLAGS ?= $(if $(filter Darwin,$(shell uname -s)),-Wl$(comma)-dead_strip,-Wl$(comma)--gc-sections)
SOURCES = src/main.c src/http_server.c src/api.c src/backend.c src/backend_mock.c src/backend_linux.c src/config_store.c src/event_bus.c src/json.c
OBJECTS = $(SOURCES:src/%.c=$(BUILD)/%.o)

.PHONY: all clean release test install
all: CPPFLAGS += -DLE_DEV_CONTROLS=1
all: $(TARGET)
$(TARGET): $(OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(OBJECTS) $(LDFLAGS) -o $@
$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD)
	$(CROSS_COMPILE)$(CC) $(CPPFLAGS) $(CSTD) $(WARN) $(CFLAGS) -Isrc -c $< -o $@
release: clean
	$(MAKE) CFLAGS="-Os -ffunction-sections -fdata-sections" LDFLAGS="$(GC_LDFLAGS)" $(TARGET)
test:
	$(MAKE) clean
	$(MAKE) all
	sh tests/run_tests.sh
install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/sbin $(DESTDIR)$(PREFIX)/share/libreecho/web $(DESTDIR)/etc/libreecho
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/sbin/libreecho-web
	cp -R web/* $(DESTDIR)$(PREFIX)/share/libreecho/web/
	install -m 0600 config/defaults.json $(DESTDIR)/etc/libreecho/web-config.json
clean:
	rm -f $(OBJECTS) $(TARGET)
