# contrib

## cacert.pem

The trust roots shipped in the release tarball, and what the runner validates
GitHub against on a machine with no SGUG-RSE.

Mozilla's root store as extracted by the curl project. It is committed rather
than fetched at release time so that a release is reproducible, so that a change
to what users trust is a reviewable diff, and because the runner's own
`download-artifact` path has never been exercised in the hosted-to-IRIX
direction.

Update it deliberately, in its own pull request:

```sh
curl -fsSLo contrib/cacert.pem https://curl.se/ca/cacert.pem
grep -c 'BEGIN CERTIFICATE' contrib/cacert.pem
```

The file's own header records the extraction date. Compare against the checksums
published at https://curl.se/docs/caextract.html before committing.

The runner prefers, in order: `SSL_CERT_FILE`, a `cert.pem` next to the binary,
then `/usr/sgug/etc/pki/tls/cert.pem`. SGUG-RSE's bundle is maintained by that
distribution and is the better source when it is present.
