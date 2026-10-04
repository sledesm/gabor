Test-only certificates for `tests/test_e2e_sim.py`: a throwaway CA and a
certificate for `localhost`. The native_sim build embeds `test_ca.der` in place
of DigiCert Global Root G2, so the test exercises the same TLS code path as the
hardware against local fakes. Never use them for anything else.
