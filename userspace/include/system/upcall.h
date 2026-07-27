#pragma once

/* Restore the context interrupted by the current upcall. */
__attribute__((noreturn))
void upcall_return(void);
