# match: setkey ...

/guestbin\/ipsec-kernel-state.sh/ b match
/^ ip xfrm state$/ b match
/^ ip xfrm state |/ b match
/^ ip -4 xfrm state$/ b match
/^ ip -6 xfrm state$/ b match
b end

:match

  # print and read next line
  n
  /^[a-z]* #/ b end

  # SPI: preserve 0

  / spi 0x00000000 /! {
	s/ spi 0x[0-9a-f]\{8\} / spi 0xSPISPI /g
  }
  s/\(\s\)spi=[1-9][0-9]*(0x[^)]*)/\1spi=SPISPI(0xSPISPI)/

  # setkey -D

  /^\s*[AE]:\s/ {
  	s/\(\s\)[0-9a-f]\{8\}/\1XXXXXXXX/g
  }
  s/\(\s\)pid=[1-9][0-9]*/\1pid=PID/
  s/\(\s\)diff: [0-9]\{1,\}/\1diff: N/

  # ipsecctl

  /authkey/ {
	s/0x[0-9a-f][0-9a-f]*/0xHASHKEY/
  }
  /enckey/ {
	s/0x[0-9a-f][0-9a-f]*/0xENCKEY/
  }
  /key_auth:/ {
	s/:\(\s\)[a-f0-9]*$/:\1HASHKEY/
  }
  /key_encrypt:/ {
	s/:\(\s\)[a-f0-9]*$/:\1ENCKEY/
  }
  /lifetime_cur:/ {
	s/ add [1-9][0-9]*/ add N/
	s/ first [1-9][0-9]*/ first N/
  }
  /lifetime_soft:/ {
	s/ bytes [1-9][0-9]* / bytes N /
	s/ add [1-9][0-9]* / add N /
  }
  /lifetime_hard:/ {
	s/ bytes [1-9][0-9]* / bytes N /
	s/ add [1-9][0-9]* / add N /
  }
  /lifetime_lastuse:/ {
	s/ first [1-9][0-9]*/ first N/
  }

  # ip xfrm state

  # some versions print the flag 80, others print esn
  /replay-window [0-9]* flag / {
	s/\( flag.*\) 80/\1 esn/
  }

  # fix up keys and other magic numbers; see also ipsec look
  s/ reqid [1-9][0-9]* / reqid REQID /g

  s/\tauth\(.*\) 0x[^ ]* \(.*\)$/\tauth\1 0xHASHKEY \2/g
  s/\tenc \(.*\) 0x.*$/\tenc \1 0xENCKEY/g
  s/\taead \(.*\) 0x[^ ]*\( .*\)$/\taead \1 0xENCAUTHKEY\2/g

b match

:end
