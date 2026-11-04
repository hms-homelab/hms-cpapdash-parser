# Third-party attributions

## cpap-lib (MIT)

Portions of the Philips Respironics PRS1 support (format facts transcribed in
`docs/PHILIPS_FORMAT.md`, the ModelNumber-to-product-name table, and the CRC-16/Kermit +
additive-8 checksum definitions in the PhilipsParser sources) are derived from
**cpap-lib** by Jonah Stagner (A.K.A. StagPoint), https://github.com/EEGKit/cpap-lib,
at commit `f760b21205e0d3f49342eacf2eed11912881aa01`.

```
MIT License

Copyright (c) 2023 Jonah Stagner (A.K.A. StagPoint)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## mattjoyce/dreamstation (Apache-2.0)

Format facts for DataFormatVersion 3, Family 0, FamilyVersion 6 in section 9 of
`docs/PHILIPS_FORMAT.md` (the v3 chunk header block, the CRC-32 form of the data checksum, and
the summary, statistics, settings and event record meanings) are transcribed from
`docs/FORMAT.md` of **dreamstation** by Matt Joyce, https://github.com/mattjoyce/dreamstation,
at commit `268d7864ddd23504ea38c0ed92bcbc992f1d20f8`, licensed under the Apache License,
Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0). Facts only were transcribed, each
checked against a real card before use; none of that project's code was read or copied.

## Apnea Board wiki

The PRS1 format description in `docs/PHILIPS_FORMAT.md` also transcribes format facts from
the Apnea Board wiki page "PR System One Data Format"
(https://www.apneaboard.com/wiki/index.php/PR_System_One_Data_Format), a public
documentation of a lawful reverse-engineering effort. Facts only were transcribed; no code
from that project's software (GPL) was consulted or used.
