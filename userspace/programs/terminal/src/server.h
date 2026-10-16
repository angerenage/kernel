#pragma once

struct terminal;

/* Advertise the terminal Stream capability and serve it indefinitely. */
int terminal_server_run(struct terminal* terminal);
