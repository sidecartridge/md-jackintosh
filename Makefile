# Version from file
VERSION := $(shell cat version.txt)

# The app settings' layout version: 2 while developing, 1 in a release.
ACONFIG_H ?= rp/src/include/aconfig.h
ACONFIG_VERSION := $(shell grep -oE 'ACONFIG_VERSION_NUMBER = [0-9A-Fa-fx]+' $(ACONFIG_H) | grep -oE '[0-9A-Fa-fx]+$$')

## Check that this tree can be released
.PHONY: check-release
check-release:
	@test "$$(printf '%d' '$(ACONFIG_VERSION)' 2>/dev/null)" = 1 || { \
	  echo "ERROR: ACONFIG_VERSION_NUMBER in $(ACONFIG_H) is '$(ACONFIG_VERSION)'; a release needs 1"; \
	  exit 1; }
	@echo "ACONFIG_VERSION_NUMBER is $(ACONFIG_VERSION)"

## Tag this version
.PHONY: tag
tag: check-release
	git tag $(VERSION) && git push origin $(VERSION) && \
	echo "Tagged: $(VERSION)"
