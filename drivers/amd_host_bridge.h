#ifndef BOOK8_AMD_HOST_BRIDGE_H
#define BOOK8_AMD_HOST_BRIDGE_H

#include "pcie.h"

bool book8_amd_host_bridge_probe(struct book8_pcie_state *);
bool book8_amd_root_link_ready(struct book8_pcie_state *);

#endif
