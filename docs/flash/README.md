# The web flasher

`index.html` flashes the badge from a browser over WebSerial, so someone who
just wants the firmware never has to install ESP-IDF.

It needs two things to work:

**HTTPS.** WebSerial is not offered to pages served over plain HTTP. GitHub
Pages provides it; serving the folder off a laptop will not.

**The three binaries next to it,** plus the `manifest.json` that lists their
flash offsets. The build workflow produces exactly that set — download the
`firmware` artifact from a run, or a release's files, and drop them in here.

To publish: enable Pages for this repository, source *Deploy from a branch*,
folder `/docs`. The page is then at
`https://<user>.github.io/amoled-badge/flash/`.

🚨 The offsets in `manifest.json` are not decoration — they have to match
`partitions.csv`. If the partition table ever moves, this file moves with it
or the board will flash cleanly and then fail to boot.
