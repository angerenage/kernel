#pragma once

#include <protocol/ahci.h>
#include <stdbool.h>
#include <stdint.h>

struct ahci_dma_buffer;
struct ahci_port;

/* Execute one ATA task-file command synchronously on slot zero. */
bool ahci_command_execute(struct ahci_port* port, const struct ahci_transport_execute_request* request,
                          struct ahci_dma_buffer* data, struct ahci_transport_execute_response* response);
