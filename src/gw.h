#pragma once

/*
 * Discord gateway over a WinHTTP WebSocket: Hello, Identify, Heartbeat, Ready.
 * gw_run() blocks until the connection closes or gw_stop() is called.
 */
int gw_run(const char *token);
void gw_stop(void);
