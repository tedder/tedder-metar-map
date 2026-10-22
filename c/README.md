# tedder-metar-map firmware (ESP-IDF)

Overview, build and flash steps: `../README.md`. Settings and the remote config file:
`../docs/config-options.md`. Implementation notes: `NOTES.md`.

| Path | What |
|------|------|
| `main/main.c` | WiFi, setup network, weather polling, LEDs, OTA, remote config, web pages |
| `main/airports.c` | airport list and weather reply parsing (plain C, host-tested) |
| `main/tx_tune.c`, `main/radio_verdict.c` | TX power autotune and antenna check logic (host-tested) |
| `main/captive_dns.c` | DNS answers for the setup network's captive page (host-tested) |
| `main/oled_ssd1306.c` | optional 128×32 OLED |
| `test/` | host tests: `make -C test` |
| `tools/` | provisioning, config and release tools |
| `partitions.csv` | two 1.875 MB OTA slots, NVS, core dump |
| `signing-pub.pem` | public half of the release signing key |
| `sdkconfig.ci` | unsigned build without the private key |
| `airports.txt.example` | an airport list (the original map, 74 LEDs) |
