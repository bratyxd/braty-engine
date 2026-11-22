# TLS test certificates

Used by `addons/tls/tests/test_tls.c` and the loopback TLS test server
(`addons/tls/tests/tls_test_server.h`) that the http and websocket tests share. For tests only:
the keys are public.

| File | What |
|---|---|
| `ca.pem` | the test CA ("Braty Test CA"); the client trusts it with `br_tls_set_ca_pem` |
| `server.pem`, `server.key` | the server certificate for `localhost` (SAN `DNS:localhost` only, so `127.0.0.1` is a name mismatch), signed by `ca.pem`; P-256 key |
| `untrusted_ca.pem` | a second CA nobody trusts ("Braty Untrusted CA") |
| `untrusted_server.pem` | the same name and key as `server.pem`, signed by `untrusted_ca.pem` |
| `server.ext` | the server certificate's extensions (input of the commands below) |

Valid until January 2109. Made once with OpenSSL 3.5 (Git Bash; `MSYS_NO_PATHCONV=1` keeps
`/CN=` from being rewritten as a path) in this folder:

```sh
export MSYS_NO_PATHCONV=1
openssl ecparam -name prime256v1 -genkey -noout -out ca.key
openssl req -x509 -new -key ca.key -sha256 -days 30000 -subj "/CN=Braty Test CA" -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.pem
openssl ecparam -name prime256v1 -genkey -noout -out untrusted_ca.key
openssl req -x509 -new -key untrusted_ca.key -sha256 -days 30000 -subj "/CN=Braty Untrusted CA" -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" -out untrusted_ca.pem
openssl ecparam -name prime256v1 -genkey -noout -out server.key
openssl req -new -key server.key -subj "/CN=localhost" -out server.csr
printf "basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost\n" > server.ext
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -sha256 -days 30000 -extfile server.ext -out server.pem
openssl x509 -req -in server.csr -CA untrusted_ca.pem -CAkey untrusted_ca.key -CAcreateserial -sha256 -days 30000 -extfile server.ext -out untrusted_server.pem
rm -f server.csr *.srl ca.key untrusted_ca.key
```

The CA keys are deleted: a new certificate means running all the commands again.
`.gitattributes` keeps `*.pem` and `*.key` byte for byte.
