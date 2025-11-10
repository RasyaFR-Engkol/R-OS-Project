#pragma once

#include "../kernel/filesys/gpt/gpt.hpp"

// Di Filesystem, kita butuh sebuah alat untuk membaca
// disk. Maka kita butuh GPT dan driver AHCI (untuk SATA).
#include "../kernel/driver/ahci/ahci.hpp"
#include "../kernel/driver/ahci/ahci_internal.hpp"
#include "../kernel/driver/ahci/ahci_regs.hpp"

#include "../../kernel/filesys/partition.hpp"
#include "../../kernel/filesys/gpt/gpt.hpp"
#include "../../kernel/filesys/pmos/partition_manager.hpp"
#include "../../kernel/filesys/filesystem.hpp"

#include "../../kernel/filesys/fat32/fat32.hpp"

// TODO: nanti disini kita bisa tambah driver NVMe juga