#ifndef PALA_GH_CA_H
#define PALA_GH_CA_H

/* Root CA for api.github.com, read off the live TLS chain rather than written
   from memory - GitHub moved from DigiCert to Sectigo, so a remembered root
   would simply have failed to connect.

   Pinned deliberately: the device carries a GitHub token, and setInsecure()
   would hand that token to anyone able to intercept the connection. The cost
   is that publishing breaks if GitHub changes certificate authority again, so
   that shows up as a reported TLS error rather than a silent no-op.

   Subject:  CN=Sectigo Public Server Authentication Root E46
   Expires:  2046-03-21 */
static const char GITHUB_ROOT_CA[] =
"-----BEGIN CERTIFICATE-----\n"
"MIICOjCCAcGgAwIBAgIQQvLM2htpN0RfFf51KBC49DAKBggqhkjOPQQDAzBfMQswCQYDVQQGEwJH\n"
"QjEYMBYGA1UEChMPU2VjdGlnbyBMaW1pdGVkMTYwNAYDVQQDEy1TZWN0aWdvIFB1YmxpYyBTZXJ2\n"
"ZXIgQXV0aGVudGljYXRpb24gUm9vdCBFNDYwHhcNMjEwMzIyMDAwMDAwWhcNNDYwMzIxMjM1OTU5\n"
"WjBfMQswCQYDVQQGEwJHQjEYMBYGA1UEChMPU2VjdGlnbyBMaW1pdGVkMTYwNAYDVQQDEy1TZWN0\n"
"aWdvIFB1YmxpYyBTZXJ2ZXIgQXV0aGVudGljYXRpb24gUm9vdCBFNDYwdjAQBgcqhkjOPQIBBgUr\n"
"gQQAIgNiAAR2+pmpbiDt+dd34wc7qNs9Xzjoq1WmVk/WSOrsfy2qw7LFeeyZYX8QeccCWvkEN/U0\n"
"NSt3zn8gj1KjAIns1aeibVvjS5KToID1AZTc8GgHHs3u/iVStSBDHBv+6xnOQ6OjQjBAMB0GA1Ud\n"
"DgQWBBTRItpMWfFLXyY4qp3W7usNw/upYTAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB\n"
"/zAKBggqhkjOPQQDAwNnADBkAjAn7qRaqCG76UeXlImldCBteU/IvZNeWBj7LRoAasm4PdCkT0RH\n"
"lAFWovgzJQxC36oCMB3q4S6ILuH5px0CMk7yn2xVdOOurvulGu7t0vzCAxHrRVxgED1cf5kDW21U\n"
"SAGKcw==\n"
"-----END CERTIFICATE-----\n";

#endif
