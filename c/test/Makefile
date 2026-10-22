CFLAGS = -Wall -Wextra -Werror -std=c11 -g -fsanitize=address,undefined

.PHONY: test
test: test_captive_dns test_tx_tune test_radio_verdict test_airports
	./test_airports
	./test_captive_dns
	./test_tx_tune
	./test_radio_verdict

test_captive_dns: test_captive_dns.c ../main/captive_dns.c ../main/captive_dns.h
	$(CC) $(CFLAGS) -o $@ test_captive_dns.c ../main/captive_dns.c

test_tx_tune: test_tx_tune.c ../main/tx_tune.c ../main/tx_tune.h
	$(CC) $(CFLAGS) -o $@ test_tx_tune.c ../main/tx_tune.c

test_radio_verdict: test_radio_verdict.c ../main/radio_verdict.c ../main/radio_verdict.h
	$(CC) $(CFLAGS) -o $@ test_radio_verdict.c ../main/radio_verdict.c

test_airports: test_airports.c ../main/airports.c ../main/airports.h
	$(CC) $(CFLAGS) -o $@ test_airports.c ../main/airports.c

clean:
	rm -f test_captive_dns test_tx_tune test_radio_verdict test_airports
