ROOT_DIR ?= $(abspath $(CURDIR))
CORE_DIR ?= $(ROOT_DIR)/core
ROOT_BUILD_DIR ?= $(ROOT_DIR)/build
ROOT_OBJ_DIR ?= $(ROOT_BUILD_DIR)/root
BUILD ?= $(ROOT_BUILD_DIR)
MODULES_DIR ?= $(ROOT_DIR)/modules
SCRIPT_DIR ?= $(ROOT_DIR)/script
SCRIPT_CONFIG_DIR ?= $(SCRIPT_DIR)/config
SCRIPT_MAKE_DIR ?= $(SCRIPT_DIR)/make
ROOT_QEMU_LOG ?= $(ROOT_DIR)/qemu.log
ROOT_OBJDUMP_LOG ?= $(ROOT_DIR)/objdump.log

-include $(CORE_DIR)/Makefile.env
-include $(CORE_DIR)/kernel/Makefile.env
-include $(CORE_DIR)/arch/Makefile.env
-include $(ROOT_DIR)/Makefile.root.env

SMP ?= 4
MEM_SIZE ?= 256M
DUMP ?= false
DBG ?= false
# User payload: set to 1 to skip git pull/fetch repair (offline).
USER_SKIP_GIT ?=

ROOT_COMPAT_ARCHS := x86_64 aarch64
ifneq ($(filter $(ARCH),$(ROOT_COMPAT_ARCHS)),)
ROOT_SOURCES := $(wildcard $(ROOT_DIR)/linux_layer/*/*.c) $(wildcard $(ROOT_DIR)/servers/*.c)
else
ROOT_SOURCES :=
endif
ROOT_OBJECTS := $(patsubst $(ROOT_DIR)/%.c,$(ROOT_OBJ_DIR)/%.o,$(ROOT_SOURCES))
ROOT_USER_OBJECT := $(ROOT_BUILD_DIR)/link_app.o
ROOT_USER_ARCH_FILE := $(ROOT_BUILD_DIR)/link_app.arch
ROOT_EXTRA_OBJECTS := $(abspath $(ROOT_OBJECTS)) $(abspath $(ROOT_USER_OBJECT))

ROOT_COMMON_CFLAGS := -Werror -Wall -Wextra -Werror=return-type -Werror=format -Wmissing-field-initializers -Wunused-result -Os -nostdlib -nostdinc -fno-builtin -fno-stack-protector -std=c11 -DNR_CPUS=$(SMP)
ROOT_COMMON_CFLAGS += -I $(ROOT_DIR)/include -I $(CORE_DIR)/include

CORE_BUILD_ARGS := ARCH=$(ARCH) SMP=$(SMP) MEM_SIZE=$(MEM_SIZE) DBG=$(DBG) DUMP=$(DUMP)
CORE_BUILD_ARGS += EXTRA_OBJECTS="$(ROOT_EXTRA_OBJECTS)"
CORE_BUILD_ARGS += QEMU_LOG="$(ROOT_QEMU_LOG)"
CORE_BUILD_ARGS += DUMPFILE="$(ROOT_OBJDUMP_LOG)"
CORE_CONFIG_ARCH := $(shell if [ -f "$(CORE_DIR)/Makefile.env" ]; then awk -F ':=[[:space:]]*' '/^ARCH[[:space:]]*:=/{print $$2; exit}' "$(CORE_DIR)/Makefile.env"; fi)
CORE_CONFIG_SMP := $(shell if [ -f "$(CORE_DIR)/Makefile.env" ]; then awk -F ':=[[:space:]]*' '/^SMP[[:space:]]*:=/{print $$2; exit}' "$(CORE_DIR)/Makefile.env"; fi)
ARCH ?= $(CORE_CONFIG_ARCH)
CC := $(CROSS_COMPLIER)gcc
LD := $(CROSS_COMPLIER)ld
AR := $(CROSS_COMPLIER)ar
OBJCOPY := $(CROSS_COMPLIER)objcopy
OBJDUMP := $(CROSS_COMPLIER)objdump

.PHONY: all root_dirs config build build_lib run clean mrproper dump show_config fmt user have_user_payload

all: build

root_dirs:
	@mkdir -p $(ROOT_BUILD_DIR) $(ROOT_OBJ_DIR)

config: mrproper root_dirs
	@if [ -z "$(ARCH)" ]; then echo "ARCH is required, for example: make ARCH=x86_64 config"; exit 1; fi
	@$(MAKE) -C $(CORE_DIR) config ARCH=$(ARCH) SMP=$(SMP) MEM_SIZE=$(MEM_SIZE) DBG=$(DBG)
	@python3 $(SCRIPT_CONFIG_DIR)/root_features.py $(ROOT_DIR) $(SCRIPT_CONFIG_DIR)/root.json

user: root_dirs
	@if [ -z "$(ARCH)" ]; then echo "ARCH is required, for example: make ARCH=x86_64 user"; exit 1; fi
	@export RENDEZVOS_USER_SKIP_GIT="$(USER_SKIP_GIT)"; \
	python3 $(SCRIPT_CONFIG_DIR)/user.py $(ARCH) $(ROOT_DIR) $(SCRIPT_CONFIG_DIR)/user.json
	@echo "User payload generated at $(ROOT_USER_OBJECT)"

have_user_payload:
	@if [ ! -f "$(ROOT_USER_OBJECT)" ]; then \
		echo "No user payload found, please run 'make user ARCH=$(ARCH)' first"; \
		exit 2; \
	fi
	@if [ ! -f "$(ROOT_USER_ARCH_FILE)" ]; then \
		echo "User payload arch file missing: $(ROOT_USER_ARCH_FILE)"; \
		echo "Please re-generate user payload: make user ARCH=$(ARCH)"; \
		exit 2; \
	fi
	@if [ "$$(cat "$(ROOT_USER_ARCH_FILE)")" != "$(ARCH)" ]; then \
		echo "User payload ARCH mismatch: have=$$(cat "$(ROOT_USER_ARCH_FILE)") want=$(ARCH)"; \
		echo "Please re-generate user payload: make user ARCH=$(ARCH)"; \
		exit 2; \
	fi

build: root_dirs
	@if [ -z "$(CORE_CONFIG_ARCH)" ] || [ "$(CORE_CONFIG_ARCH)" != "$(ARCH)" ] || [ -z "$(CORE_CONFIG_SMP)" ] || [ "$(CORE_CONFIG_SMP)" != "$(SMP)" ]; then \
		$(MAKE) -C $(CORE_DIR) mrproper; \
		$(MAKE) config ARCH=$(ARCH) SMP=$(SMP) MEM_SIZE=$(MEM_SIZE) DBG=$(DBG) && \
		$(MAKE) build ARCH=$(ARCH) SMP=$(SMP) MEM_SIZE=$(MEM_SIZE) DBG=$(DBG); \
	else \
		$(MAKE) build_lib $(CORE_BUILD_ARGS); \
	fi

build_lib: have_user_payload $(ROOT_OBJECTS)
	@$(MAKE) -C $(CORE_DIR) all $(CORE_BUILD_ARGS)

run: build
	@$(MAKE) -C $(CORE_DIR) run $(CORE_BUILD_ARGS)

show_config:
	@$(MAKE) -C $(CORE_DIR) show_config ARCH=$(ARCH) SMP=$(SMP) MEM_SIZE=$(MEM_SIZE) DBG=$(DBG)

fmt:
	@if ! command -v clang-format >/dev/null 2>&1; then echo "clang-format not found"; exit 1; fi
	@{ \
		git ls-files '*.c' '*.h'; \
		git ls-files --others --exclude-standard '*.c' '*.h'; \
	} | sort -u | while IFS= read -r f; do \
		if [ -f "$$f" ]; then clang-format -i -style=file "$$f"; fi; \
	done

dump: build
	@$(MAKE) -C $(CORE_DIR) dump $(CORE_BUILD_ARGS)

clean:
	@$(MAKE) -C $(CORE_DIR) clean
	@-rm -rf $(ROOT_BUILD_DIR)

mrproper:
	@$(MAKE) -C $(CORE_DIR) mrproper
	@-rm -rf $(ROOT_BUILD_DIR)

$(ROOT_OBJ_DIR)/%.o: $(ROOT_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC	" $@
	@$(CC) $(CFLAGS) $(ROOT_COMMON_CFLAGS) $(ROOT_EXTRA_CFLAGS) -c $< -o $@
