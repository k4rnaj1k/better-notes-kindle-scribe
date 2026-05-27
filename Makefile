DOCKER_IMAGE = betternotes-sdk
TARGET ?= kindlehf
BIN     = builddir_kindle/betternotes

.PHONY: all build docker-image clean dist

all: build

docker-image:
	docker build --target toolchain -t betternotes-toolchain .
	docker rm -f sdk-builder 2>/dev/null || true
	docker run --privileged --name sdk-builder -u builder betternotes-toolchain \
		/bin/sh -c "cd ~/kindle-sdk && ./gen-sdk.sh $(TARGET)"
	docker commit sdk-builder $(DOCKER_IMAGE)
	docker rm sdk-builder

build:
	docker run --rm -v "$(CURDIR)":/src $(DOCKER_IMAGE) sh -c '\
		cd /src && rm -rf builddir_kindle && \
		meson setup --cross-file $$MESON_CROSS builddir_kindle && \
		meson compile -C builddir_kindle'

dist: build
	rm -rf dist
	mkdir -p dist/extensions/betternotes/bin dist/extensions/betternotes/data
	cp $(BIN) dist/extensions/betternotes/bin/
	cp kual/betternotes/config.xml kual/betternotes/menu.json dist/extensions/betternotes/
	cp kual/betternotes/bin/*.sh dist/extensions/betternotes/bin/
	@echo "dist/extensions/betternotes ready — copy into /mnt/us/extensions/ on the Kindle"

clean:
	rm -rf builddir_kindle dist
