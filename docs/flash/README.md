# The web flasher

`index.html` flashes the badge from a browser over WebSerial, so someone who
just wants the firmware never has to install ESP-IDF.

It needs two things to work:

**HTTPS.** WebSerial is not offered to pages served over plain HTTP. GitHub
Pages provides it; serving the folder off a laptop will not.

**The three binaries next to it,** plus the `manifest.json` that lists their
flash offsets. They are not in the repository and should not be — a 3.6 MB
image committed on every firmware change goes stale without anyone noticing,
and a stranger ends up flashing an old build. `.github/workflows/build.yml`
copies them in as it publishes, so the page always offers what the current
commit builds.

To publish: enable Pages for this repository with source **GitHub Actions**
(not *Deploy from a branch* — that would serve `docs/` as it sits in git,
without the binaries). The page is then at
`https://<user>.github.io/The-Badge/flash/`, rebuilt on every push to
`main`.

🚨 The offsets in `manifest.json` are not decoration — they have to match
`partitions.csv`. If the partition table ever moves, this file moves with it
or the board will flash cleanly and then fail to boot.
