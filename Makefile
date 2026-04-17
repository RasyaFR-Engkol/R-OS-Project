# Makefile for ROS (Rasya OS)
# Automatically compiles all .c, .cpp, and .asm files in the project
# Modified for Linux-style "pretty" output

AS := nasm
CXX := x86_64-elf-g++
CC  := x86_64-elf-gcc
LD := x86_64-elf-ld

BUILD_ACPICA := 0

ifeq ($(BUILD_ACPICA),1)
INCLUDE_ACPICA := -Ifirmware/acpica/source/include -Ifirmware/acpica/source/components
else
INCLUDE_ACPICA :=
endif

CXXFLAGS := -g -std=gnu++20 -ffreestanding -fno-exceptions -fno-rtti -m64 -Os \
 -Wall -Wextra -Wpedantic -Werror \
 -Wshadow -Wpointer-arith -Wcast-align -Wundef -Wstrict-overflow=5 -Wno-strict-overflow \
 -Wno-unused-parameter -Wno-unused-function \
 -IInclude -mcmodel=kernel -fno-pic -fno-pie \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-omit-frame-pointer \
 -mno-red-zone \
 -fstack-protector-strong \
 -mno-mmx -mno-sse -mno-avx \
 -ffunction-sections -fdata-sections \
 -Wformat-security -fno-common -fno-strict-aliasing -mgeneral-regs-only \
 -D__ROS_KERNEL__ -DACPI_MACHINE_WIDTH=64 -DACPI_NO_ERROR_MESSAGES=1 -DACPI_SINGLE_THREADED \
 -DACPI_USE_DO_WHILE_0 -DACPI_DEBUG_OUTPUT=0 $(INCLUDE_ACPICA)

# ACPICA core is C, not C++. We'll compile with a relaxed warning set.
ACPICA_CFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -Os \
 -mcmodel=kernel \
 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
 -Wno-format -Wno-unused-variable -Wno-unused-function -Wno-cast-align \
 -DACPI_MACHINE_WIDTH=64 -DACPI_NO_ERROR_MESSAGES=1 -DACPI_SINGLE_THREADED \
 -DACPI_USE_DO_WHILE_0 -DACPI_DEBUG_OUTPUT=0 -D__ROS_KERNEL__ \
 $(INCLUDE_ACPICA) -IInclude

LDFLAGS := -T x86_64/linkers.ld \
-nostdlib \
-z max-page-size=0x1000 \
-gc-sections \


ASFLAGS := -f elf64

BUILD_DIR := build
TARGET := $(BUILD_DIR)/kernel.elf
CXX = ~/opt/cross/bin/x86_64-elf-g++

# Temukan semua file sumber (exclude build directory to avoid generated files)
C_SRCS := $(shell find . -path ./$(BUILD_DIR) -prune -o -type f -name "*.c" -print | \
          grep -v "firmware/acpica/source/" | \
          grep -v "exported_driver/")

ifeq ($(BUILD_ACPICA),1)
ACPICA_SRCS := \
	$(shell find firmware/acpica/source/components -type f -name "*.c" \
		| grep -v "/debugger/" \
		| grep -v "/disassembler/" \
		| grep -v "nsdumpdv.c")
else
ACPICA_SRCS :=
endif

CPP_SRCS := $(shell find . -path ./$(BUILD_DIR) -prune -o -type f -name "*.cpp" -print | \
            grep -v "exported_driver/")
# All ASM sources except the AP trampoline which is a flat binary
ASM_SRCS := $(shell find . -path ./$(BUILD_DIR) -prune -o -type f -name "*.asm" -print | \
            grep -v "firmware/acpi/madt/smpmod/rmpmlmtramp.asm")

# Ubah jadi object file path di build/
C_OBJS := $(patsubst ./%, $(BUILD_DIR)/%, $(C_SRCS:.c=.o))
ACPICA_OBJS := $(patsubst firmware/%, $(BUILD_DIR)/firmware/%, $(ACPICA_SRCS:.c=.o))
CPP_OBJS := $(patsubst ./%, $(BUILD_DIR)/%, $(CPP_SRCS:.cpp=.o))
ASM_OBJS := $(patsubst ./%, $(BUILD_DIR)/%, $(ASM_SRCS:.asm=.o))


# ================================================
# FOR KO MODULES (MULTI-FILE FOLDER STRUCTURE)
# ================================================

# Kita definisikan manual supaya bersih dari -fno-pic kernel
KMOD_CXXFLAGS_BASE := -g -std=gnu++20 -ffreestanding -fno-exceptions -fno-rtti -m64 -Os \
				 -Wall -Wextra -IInclude -mno-red-zone -mgeneral-regs-only \
				 -D__ROS_KERNEL__ -fPIC -mcmodel=large

# LDFLAGS: Tambahkan -pie atau -shared secara eksplisit ke linker
KMOD_LDFLAGS := -Wl,-shared -Wl,-pie -nostdlib -z max-page-size=0x1000 \
				-Wl,-e,module_init -Wl,--fatal-warnings

# 1. Cari semua sub-folder di dalam exported_driver/ (misal: exported_driver/xhci/)
KMOD_DIRS := $(wildcard exported_driver/*/)
# Ekstrak nama foldernya saja (misal: xhci)
KMOD_NAMES := $(patsubst exported_driver/%/,%,$(KMOD_DIRS))

# 2. Tentukan target akhir: build/exported_driver/xhci.ko, build/exported_driver/e1000.ko
KMOD_TARGETS := $(patsubst %, $(BUILD_DIR)/exported_driver/%.ko, $(KMOD_NAMES))

# 3. MACRO: Bikin aturan kompilasi dinamis untuk setiap folder driver
define MAKE_DRIVER_RULE
# Ambil semua file .cpp dan .c di folder driver ini (misal di exported_driver/$1/)
$1_SRCS := $$(shell find exported_driver/$1 -type f -name "*.cpp" -o -name "*.c")

# Bikin list target .o yang akan ditaruh di build/exported_driver/$1/
$1_OBJS := $$(patsubst exported_driver/%.cpp, $$(BUILD_DIR)/exported_driver/%.o, $$(filter %.cpp, $$($1_SRCS))) \
		   $$(patsubst exported_driver/%.c, $$(BUILD_DIR)/exported_driver/%.o, $$(filter %.c, $$($1_SRCS)))

# Rule Linker: Gabungkan SEMUA .o di folder ini jadi SATU .ko (misal: $1.ko)
$$(BUILD_DIR)/exported_driver/$1.ko: $$($1_OBJS)
	@mkdir -p $$(dir $$@)
	@echo "  [KMOD-LD]  $$@ (Linked from $1/)"
	@$$(CXX) $$(KMOD_CXXFLAGS_BASE) -shared $$^ -o $$@ $$(KMOD_LDFLAGS)
endef

# 4. Eksekusi Macro di atas untuk semua nama driver yang terdeteksi
$(foreach drv, $(KMOD_NAMES), $(eval $(call MAKE_DRIVER_RULE,$(drv))))

# AP Trampoline (flat binary at 0x8000). We assemble both a flat binary and an
# embedded object so the kernel can memcpy it into low memory automatically.
TRAMP_SRC := firmware/acpi/madt/smpmod/rmpmlmtramp.asm
TRAMP_BIN := $(BUILD_DIR)/firmware/acpi/madt/smpmod/rmpmlmtramp.bin
TRAMP_OBJ := $(BUILD_DIR)/firmware/acpi/madt/smpmod/rmpmlmtramp_bin.o

OBJS := $(C_OBJS) $(CPP_OBJS) $(ASM_OBJS) $(TRAMP_OBJ) $(ACPICA_OBJS)


# Default rule 
# --- UBAH BAGIAN INI ---
# Default rule (Sekarang ngebangun kernel DAN semua driver)
all: $(TRAMP_BIN) $(TRAMP_OBJ) $(TARGET) $(KMOD_TARGETS)

# Linking
$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	@echo "  [LD]     $@"
	@$(LD) $(LDFLAGS) -o $@ $^
	# Generate kernel symbol table from the linked ELF and relink with it
	@echo "  [GEN]     $(BUILD_DIR)/kernsym.c"
	@python3 tools/gen-kernsym.py $@ $(BUILD_DIR)/kernsym.c || true
	@if [ -f $(BUILD_DIR)/kernsym.c ]; then \
		echo "  [G++]     $(BUILD_DIR)/kernsym.c"; \
		$(CXX) $(CXXFLAGS) -c $(BUILD_DIR)/kernsym.c -o $(BUILD_DIR)/kernsym.o && \
		echo "  [LD]      $@ (relink w/ kernsym)"; \
		$(LD) $(LDFLAGS) -o $@ $^ $(BUILD_DIR)/kernsym.o; \
	fi

# Compile rules
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  [G++]     $<"
	@$(CXX) $(CXXFLAGS) -x c++ -c $< -o $@

# ACPICA C objects (pure C, different flags) — use C compiler
$(BUILD_DIR)/firmware/acpica/%.o: firmware/acpica/%.c
	@mkdir -p $(dir $@)
	@echo "  [CC]      $<"
	@$(CC) $(ACPICA_CFLAGS) -c $< -o $@

# Loosen warnings for C++ files under firmware/acpi/** that include ACPICA headers
$(BUILD_DIR)/firmware/acpi/%.o: firmware/acpi/%.cpp
	@mkdir -p $(dir $@)
	@echo "  [G++]     $<"
	@$(CXX) $(CXXFLAGS) -Wno-pedantic -Wno-error=pedantic -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  [G++]     $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# 5. Rule Compiler Individual: Ubah .cpp/.c milik modul jadi .o
# PENTING: Harus pakai KMOD_CXXFLAGS_BASE supaya dapat -fPIC
$(BUILD_DIR)/exported_driver/%.o: exported_driver/%.cpp
	@mkdir -p $(dir $@)
	@echo "  [KMOD-G++] $<"
	@$(CXX) $(KMOD_CXXFLAGS_BASE) -c $< -o $@

$(BUILD_DIR)/exported_driver/%.o: exported_driver/%.c
	@mkdir -p $(dir $@)
	@echo "  [KMOD-CC]  $<"
	@$(CXX) $(KMOD_CXXFLAGS_BASE) -x c++ -c $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  [AS]      $<"
	@$(AS) $(ASFLAGS) $< -o $@

# Build AP trampoline as a flat binary to honor ORG 0x8000 in the ASM
$(TRAMP_BIN): $(TRAMP_SRC)
	@mkdir -p $(dir $@)
	@echo "  [AS-BIN]  $<"
	@$(AS) -f bin $< -o $@

$(TRAMP_OBJ): $(TRAMP_BIN)
	@mkdir -p $(dir $@)
	@echo "  [EMBED]   $< -> $@"
	@$(LD) -r -b binary -o $@ $<

# (Optional) To wrap the binary into an ELF for debugging/tools, run manually:
#   $(LD) -r -b binary -o $(TRAMP_BIN:.bin=.o) $(TRAMP_BIN)
#   $(LD) -T x86_64/ap_trampoline.ld -o $(TRAMP_BIN:.bin=.elf) $(TRAMP_BIN:.bin=.o)

clean:
	@echo "  [CLEAN]   Removing build directory ($(BUILD_DIR))"
	@rm -rf $(BUILD_DIR)
	# kernsym files are inside BUILD_DIR, so they are removed automatically.

.PHONY: all clean