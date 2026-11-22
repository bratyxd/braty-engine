# cacert.pem

`cacert.pem` is the Mozilla CA certificate bundle as extracted by the curl project
(https://curl.se/docs/caextract.html) from Mozilla's `certdata.txt` (NSS). It is compiled into
the `tls` add-on (`braty_tls`) as the built-in root certificates.

The certificate data is licensed under the Mozilla Public License, version 2.0
(https://mozilla.org/MPL/2.0/). It is used unmodified; its source form is the file
`addons/tls/certs/cacert.pem` in the Braty Engine repository and https://curl.se/ca/cacert.pem.
