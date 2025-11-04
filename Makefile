# Makefile for ROS (Rasya OS)
# Automatically compiles all .c, .cpp, and .asm files in the project

AS := nasm
CXX := x86_64-elf-g++
LD := x86_64-elf-ld

CXXFLAGS := -g -std=gnu++20 -ffreestanding -fno-exceptions -fno-rtti -m64 -O2 \
-Wall -Wextra -Wpedantic -Werror \
-Wshadow -Wpointer-arith -Wcast-align -Wundef -Wstrict-overflow=5 \
-Wno-unused-parameter -Wno-unused-function \
-IInclude -mcmodel=kernel -fno-pic -fno-pie \
-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-omit-frame-pointer \
-mno-red-zone \
\
-fstack-protector-strong \
-mno-mmx -mno-sse -mno-avx \
\
-ffunction-sections -fdata-sections \
-Wformat-security

LDFLAGS := -T x86_64/linkers.ld \
-nostdlib \
-z max-page-size=0x1000 \

ASFLAGS := -f elf64

BUILD_DIR := build
TARGET := $(BUILD_DIR)/kernel.elf

# Temukan semua file sumber (exclude build directory to avoid generated files)
C_SRCS := $(shell find . -path ./$(BUILD_DIR) -prune -o -type f -name "*.c" -print)
CPP_SRCS := $(shell find . -path ./$(BUILD_DIR) -prune -o -type f -name "*.cpp" -print)
ASM_SRCS := $(shell find . -path ./$(BUILD_DIR) -prune -o -type f -name "*.asm" -print)

# Ubah jadi object file path di build/
C_OBJS := $(patsubst ./%, $(BUILD_DIR)/%, $(C_SRCS:.c=.o))
CPP_OBJS := $(patsubst ./%, $(BUILD_DIR)/%, $(CPP_SRCS:.cpp=.o))
ASM_OBJS := $(patsubst ./%, $(BUILD_DIR)/%, $(ASM_SRCS:.asm=.o))

OBJS := $(C_OBJS) $(CPP_OBJS) $(ASM_OBJS)

# Default rule 
all: $(TARGET)

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

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

clean:
	rm -rf $(BUILD_DIR)
	# Also remove generated symbol C so next build regenerates cleanly
	if [ -f $(BUILD_DIR)/kernsym.c ]; then rm -f $(BUILD_DIR)/kernsym.c $(BUILD_DIR)/kernsym.o; fi

.PHONY: all clean
