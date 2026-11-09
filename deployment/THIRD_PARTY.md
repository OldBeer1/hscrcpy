# Third-party components

This local migration package retains the original hscrcpy Apache-2.0 license. The desktop binary links a GPL-enabled FFmpeg build; FFmpeg and its included components retain their own licenses. The complete combined distribution must respect those licenses; the Apache notice is not a replacement for FFmpeg's GPL terms.

- hscrcpy: https://gitcode.com/pengxiaochuan/hscrcpy ; project source snapshot in source/.
- FFmpeg: N-124795-gc27a3b12e3-20260604, BtbN GPL shared build. Core source: https://github.com/FFmpeg/FFmpeg/tree/c27a3b12e3 ; build system/dependency recipes: https://github.com/BtbN/FFmpeg-Builds . GPL text in GPL-3.0.txt. FFmpeg DLLs may be replaced by compatible shared builds.
- nghttp2 1.71.0: MIT, nghttp2-COPYING, https://github.com/nghttp2/nghttp2 .
- nlohmann/json: MIT, nlohmann-LICENSE.MIT, https://github.com/nlohmann/json .
- OpenHarmony HDC, hap-sign-tool, SDK permission metadata: retained OpenHarmony-SDK-NOTICE.txt, https://gitcode.com/openharmony/developtools_hdc and https://gitcode.com/openharmony/developtools_hapsigner .
- libusb 1.0.28: LGPL-2.1-or-later; SDK notice contains the license, https://github.com/libusb/libusb .
- Node 24.14.1: Node-LICENSE.txt and embedded dependency notices, https://nodejs.org .
- Huawei @deveco/deveco-cli 1.3.0-stable: MIT, DevEco-CLI-LICENSE and DevEco-CLI-DEPENDENCIES. Sign-password-material decoding in deployment/phone-tools.mjs follows this version's MIT-licensed implementation. https://www.npmjs.com/package/@deveco/deveco-cli .
- JetBrains Runtime 25.0.2: full legal notices retained under tools/java/legal and tools/java/release, https://github.com/JetBrains/JetBrainsRuntime .
- Existing server libraries and resources retain their upstream provenance and notices.

No personal signing key, password, device Profile or account cache is intentionally included. The toolkit creates device-specific signing material only after the operator logs in and authorizes Huawei's service.
