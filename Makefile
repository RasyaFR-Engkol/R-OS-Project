# Makefile for ROS (Rasya OS)
# Automatically compiles all .c, .cpp, and .asm files in the project

AS := nasm
CXX := x86_64-elf-g++
CC  := x86_64-elf-gcc
LD := x86_64-elf-ld

INCLUDE_ACPICA := -Ifirmware/acpica/source/include -Ifirmware/acpica/source/components

CXXFLAGS := -g -std=gnu++20 -ffreestanding -fno-exceptions -fno-rtti -m64 -O2 \
 -Wall -Wextra -Wpedantic -Werror \
 -Wshadow -Wpointer-arith -Wcast-align -Wundef -Wstrict-overflow=5 \
 -Wno-unused-parameter -Wno-unused-function \
 -IInclude -mcmodel=kernel -fno-pic -fno-pie \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-omit-frame-pointer \
 -mno-red-zone \
 -fstack-protector-strong \
 -mno-mmx -mno-sse -mno-avx \
 -ffunction-sections -fdata-sections \
 -Wformat-security -fno-common \
 -D__ROS_KERNEL__ -DACPI_MACHINE_WIDTH=64 -DACPI_NO_ERROR_MESSAGES=1 -DACPI_SINGLE_THREADED \
 -DACPI_USE_DO_WHILE_0 -DACPI_DEBUG_OUTPUT=0 $(INCLUDE_ACPICA)

# ACPICA core is C, not C++. We'll compile with a relaxed warning set.
ACPICA_CFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
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
CXX = /home/rasya/cross/bin/x86_64-elf-g++

# Temukan semua file sumber (exclude build directory to avoid generated files)
C_SRCS := $(shell find . -path ./$(BUILD_DIR) -prune -o -type f -name "*.c" -print | \
		  grep -v "firmware/acpica/source/")
ACPICA_SRCS := \
	$(shell find firmware/acpica/source/components -type f -name "*.c" \
		| grep -v "/debugger/" \
		| grep -v "/disassembler/" \
		| grep -v "nsdumpdv.c")

CPP_SRCS := $(shell find . -path ./$(BUILD_DIR) -prune -o -type f -name "*.cpp" -print)
# All ASM sources except the AP trampoline which is a flat binary
ASM_SRCS := $(shell find . -path ./$(BUILD_DIR) -prune -o -type f -name "*.asm" -print | \
			grep -v "firmware/acpi/madt/smpmod/rmpmlmtramp.asm")

# Ubah jadi object file path di build/
C_OBJS := $(patsubst ./%, $(BUILD_DIR)/%, $(C_SRCS:.c=.o))
ACPICA_OBJS := $(patsubst firmware/%, $(BUILD_DIR)/firmware/%, $(ACPICA_SRCS:.c=.o))
CPP_OBJS := $(patsubst ./%, $(BUILD_DIR)/%, $(CPP_SRCS:.cpp=.o))
ASM_OBJS := $(patsubst ./%, $(BUILD_DIR)/%, $(ASM_SRCS:.asm=.o))

# AP Trampoline (flat binary at 0x8000). We assemble both a flat binary and an
# embedded object so the kernel can memcpy it into low memory automatically.
TRAMP_SRC := firmware/acpi/madt/smpmod/rmpmlmtramp.asm
TRAMP_BIN := $(BUILD_DIR)/firmware/acpi/madt/smpmod/rmpmlmtramp.bin
TRAMP_OBJ := $(BUILD_DIR)/firmware/acpi/madt/smpmod/rmpmlmtramp_bin.o

OBJS := $(C_OBJS) $(CPP_OBJS) $(ASM_OBJS) $(TRAMP_OBJ) $(ACPICA_OBJS)


# Default rule 
all: $(TRAMP_BIN) $(TRAMP_OBJ) $(TARGET)

# Linking
$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^
	@echo "Linked $@"
	# Generate kernel symbol table from the linked ELF and relink with it
	@python3 tools/gen-kernsym.py $@ $(BUILD_DIR)/kernsym.c || true
	@if [ -f $(BUILD_DIR)/kernsym.c ]; then \
		$(CXX) $(CXXFLAGS) -c $(BUILD_DIR)/kernsym.c -o $(BUILD_DIR)/kernsym.o && \
		$(LD) $(LDFLAGS) -o $@ $^ $(BUILD_DIR)/kernsym.o && \
		echo "Relinked $@ with kernsym"; \
	fi

# Compile rules
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -x c++ -c $< -o $@

# ACPICA C objects (pure C, different flags) — use C compiler
$(BUILD_DIR)/firmware/acpica/%.o: firmware/acpica/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ACPICA_CFLAGS) -c $< -o $@

# Loosen warnings for C++ files under firmware/acpi/** that include ACPICA headers
$(BUILD_DIR)/firmware/acpi/%.o: firmware/acpi/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Wno-pedantic -Wno-error=pedantic -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# Build AP trampoline as a flat binary to honor ORG 0x8000 in the ASM
$(TRAMP_BIN): $(TRAMP_SRC)
	@mkdir -p $(dir $@)
	$(AS) -f bin $< -o $@
	@echo "Built AP trampoline binary: $@"

$(TRAMP_OBJ): $(TRAMP_BIN)
	@mkdir -p $(dir $@)
	$(LD) -r -b binary -o $@ $<
	@echo "Embedded AP trampoline object: $@"

# (Optional) To wrap the binary into an ELF for debugging/tools, run manually:
#   $(LD) -r -b binary -o $(TRAMP_BIN:.bin=.o) $(TRAMP_BIN)
#   $(LD) -T x86_64/ap_trampoline.ld -o $(TRAMP_BIN:.bin=.elf) $(TRAMP_BIN:.bin=.o)

clean:
	rm -rf $(BUILD_DIR)
	# Also remove generated symbol C so next build regenerates cleanly
	if [ -f $(BUILD_DIR)/kernsym.c ]; then rm -f $(BUILD_DIR)/kernsym.c $(BUILD_DIR)/kernsym.o; fi

.PHONY: all clean
