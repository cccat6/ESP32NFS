# Releases and Arduino Library Manager

## GitHub Actions

The CI workflow checks Arduino library metadata and compiles every example for ESP32, ESP32-C3, ESP32-C6 and ESP32-S3. It automatically selects the Arduino Lint submission or update mode from the current library index.

For a release:

1. Update `version` in `library.properties` and commit the release changes to `main`.
2. Open **Actions → Release → Run workflow** and enter that version, such as `1.0.0`.
3. The workflow validates the version, runs CI, creates the version tag and publishes the GitHub Release.

Pushing a matching `v1.0.0`-style tag also starts the release workflow. Each release contains the installable library ZIP, SHA-256 checksums and automatically generated release notes. The ZIP is created from the tagged source tree.

## Arduino Library Manager registration

Arduino Library Manager uses the public [Arduino Library Registry](https://github.com/arduino/library-registry).

Initial registration consists of adding this repository URL to `repositories.txt` through a pull request:

```text
https://github.com/cccat6/ESP32NFS
```

The registry automation checks the repository, metadata and published version tag, then processes the registration. Its [submission guide](https://github.com/arduino/library-registry#adding-a-library-to-library-manager) describes the workflow.

After registration, Arduino's indexer checks registered repositories for new version tags every hour. Updating `library.properties` and publishing a matching release through the workflow makes each new version eligible for automatic indexing. Users receive it through Arduino IDE and Arduino CLI's library-management features.

Indexing status is available in the [Arduino indexer log](https://downloads.arduino.cc/libraries/logs/github.com/cccat6/ESP32NFS/). The [registry FAQ](https://github.com/arduino/library-registry/blob/main/FAQ.md) explains metadata, versioning and updates.
