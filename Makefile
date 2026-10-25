# RTL8188EAirport — Native macOS Sequoia Airport Driver for RTL8188EUS

SHELL := /bin/bash
ROOT  := $(shell pwd)

.PHONY: all fetch build check clean install

all: build

fetch:
	@./scripts/fetch-upstream.sh

build:
	@./scripts/build-m4.sh

check:
	@./scripts/check-device.sh

clean:
	@rm -rf src/driver/build src/driver/build-m4 src/build-test
	@echo "Cleaned build artifacts."
