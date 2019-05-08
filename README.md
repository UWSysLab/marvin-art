# Marvin ART

This repository is a fork of the Android Open Source Project's `art` repository
(tag `android-7.1.1_r57`) modified to implement our prototype of the Marvin
Runtime (MRT). It is compatible with ARM64 Android devices such as the Pixel XL.

## Compiling Marvin

1. Download the AOSP source tree according to the
[instructions][downloading-aosp] on the AOSP website. When running `repo init`,
use the argument `-b android-7.1.1_r57`.

2. Replace the contents of the `art` directory with the contents of this repo.

3. Replace the contents of the `libcore` directory with the contents of the
Marvin libcore repo (which should be hosted on the same site as this repo).

4. Compile AOSP using the [instructions][compiling-aosp] on the AOSP website.

5. Deploy your build onto an ARM64 Android device. If you can plug the device
directly into the computer where you compiled AOSP, you can use
`fastboot flashall` as recommended in the [instructions][flashing-aosp] on the
AOSP website. Otherwise, you can copy the `.img` files from the
`out/target/product/[product-name]` directory of the AOSP source tree and
manually flash them onto your device using the version `fastboot` that comes
with the Android SDK.

[downloading-aosp]: https://source.android.com/setup/build/downloading
[compiling-aosp]: https://source.android.com/setup/build/building
[flashing-aosp]: https://source.android.com/setup/build/running