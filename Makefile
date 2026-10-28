# Builds the Schatkamer payload. Needs PS5_PAYLOAD_SDK, CURL_CFLAGS and CURL_LIBS (see build.sh).
PS5_HOST ?= 192.168.1.199
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

# One payload: it installs the web player app, then serves the web UI and relays the streams.
ELF := schatkamer-app.elf

SOURCES := src/app.c src/install.c src/console_curl.c
LINK := -Wl,--start-group $(CURL_LIBS) -Wl,--end-group -lpthread -lSceNet -lSceIpmi -lSceAppInstUtil
CFLAGS_ALL := -Wall -Werror -g -Ddladdr=app_unused_dladdr $(CURL_CFLAGS) -Isrc -Ibuild -Wl,--wrap=fcntl

all: $(ELF)

build/shaka.inc: third_party/shaka-player.compiled.js tools/embed.py
	mkdir -p build
	python3 tools/embed.py shaka $< $@

build/webui.inc: src/webui.html tools/embed.py
	mkdir -p build
	python3 tools/embed.py webui $< $@

$(ELF): $(SOURCES) build/shaka.inc build/webui.inc launcher/param.json launcher/icon0.png launcher/pic1.png
	$(CC) $(CFLAGS_ALL) -o $@ $(SOURCES) $(LINK)

clean:
	rm -rf build $(ELF)

# Sends the installer to the PS5's ELF loader.
install: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
