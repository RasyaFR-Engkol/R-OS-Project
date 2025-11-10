#pragma once

#include "../../kernel/driver/pci/pci.hpp"
#include "../../kernel/driver/pci/capatibility/msixmsi/msixmsi.hpp"
#include "../../kernel/driver/xhci/xhci.hpp"
#include "../../kernel/driver/xhci/xhci_internal.hpp"
#include "../../kernel/driver/xhci/xhci_isr.hpp"
#include "../../kernel/driver/xhci/xhci_regs.hpp"
#include "../../kernel/driver/ahci/ahci.hpp"
#include "../../kernel/driver/ahci/ahci_internal.hpp"
#include "../../kernel/driver/ahci/ahci_regs.hpp"
#include <rossys.hpp>
#include <rosval.h>