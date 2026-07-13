#pragma once

// Onboard WS2812 status LED. Blinks as a heartbeat; color shows modem state:
//   red    — modem not responding
//   yellow — modem up, SIM/registration pending
//   green  — registered on the network
//   blue   — PDP context active (has an IP)
void led_init(void);
