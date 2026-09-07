# Prerelease validation

## Completed

- Fresh release branch based on public develop commit f529900.
- Shipping snapshot transferred without development commit ancestry.
- Personal research documents, captured evidence, tool directories, bridge transport,
  debug loader and synthetic-guitar source excluded from the new snapshot.
- Public native and managed build passed.
- All 12 audio fixtures (three rates; silence, noise, low E and B19) matched the
  established reference output byte for byte from the three-file package.
- An old managed DLL deliberately placed beside the GUI did not override the
  main-folder rsmodsplus.dll; the B19 regression passed.
- Package contains exactly the native DLL, managed DLL and GUI executable.

## Pending

- Install this clean-branch build and confirm GUI, automatic ML startup and live
  Note by Note behavior, including fast repeated notes, bends and chords.
- Verify the intended audio routes and a clean Windows installation.
- Choose the release version and complete the release notes after gameplay acceptance.
- Push and publication require separate authorization.

Existing public-base documents remain inherited; the new snapshot does not add
private investigation files. The local development checkout is preserved.
